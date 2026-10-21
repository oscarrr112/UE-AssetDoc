// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentBlackboardFootprintProjection.h"
#include "AssetDocumentCoreRuntimeComposition.h"
#include "AssetDocumentPythonRuntime.h"
#include "AssetDocumentService.h"
#include "AssetDocumentRuntimeTargetActivation.h"
#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "AssetDocumentMutationComposition.h"

class FAssetDocumentBlackboardAtomicTransactionAdapter;
class FAssetDocumentAnimationRequestBinding;
class FAssetDocumentStagedMontageReadBinding;
class FAssetDocumentStagedBehaviorTreeReadBinding;

class FAssetDocumentGenericRuntimeRoute final
	: public IAssetDocumentGenericRuntimeRoute
	, public IAssetDocumentPythonRuntimeHost
{
public:
	FAssetDocumentGenericRuntimeRoute();
	explicit FAssetDocumentGenericRuntimeRoute(
		IAssetDocumentPythonRuntime& InRuntime);
	~FAssetDocumentGenericRuntimeRoute();

	FAssetDocumentContractValidationResult BindRuntimeTargetActivations(
		TArray<FAssetDocumentRuntimeTargetActivation>&& Activations);
	FAssetDocumentRuntimeValueDto DispatchDescribeRegisteredProfiles() const;

	void BindMutationPreparationProjection(TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> Projection);
	void BindAnimationRequestProvider(TSharedPtr<FAssetDocumentAnimationRequestBinding> Binding,
		TSharedPtr<FAssetDocumentStagedMontageReadBinding> StagedReadBinding);
 void BindBehaviorTreeStagedReadProvider(TSharedPtr<FAssetDocumentStagedBehaviorTreeReadBinding> Binding);
 FAssetDocumentPrimitiveOutcome InvokePreparedMutation(const FString& ProviderId,
  const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
  IAssetDocumentAtomicTransaction* Transaction, TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward);

 void BindComposition(FAssetDocumentCoreRuntimeComposition& InComposition);
	void UnbindComposition();

	virtual FAssetDocumentResult DispatchApplyFile(
		const FAssetDocumentApplyFileRequest& Request) override;
	virtual FAssetDocumentResult DispatchValidate(
		const FAssetDocumentValidateRequest& Request) override;
	virtual FAssetDocumentRuntimeResultDto DispatchExtract(
		const FAssetDocumentExtractRequest& Request,
		const FString& AssetClassPath) override;
	virtual FAssetDocumentRuntimeResultDto DispatchDiff(
		const FAssetDocumentDiffRequest& Request) override;
	virtual FAssetDocumentRuntimeResultDto DispatchInspectProfile(
		const FAssetDocumentProfileRequest& Request,
		const FString& AssetClassPath) override;
	virtual FAssetDocumentRuntimeResultDto DispatchInspect(
		const FAssetDocumentInspectRequest& Request,
		const FString& AssetClassPath,
		const FString& AssetObjectPath) override;
	virtual FAssetDocumentRuntimeResultDto DispatchCreateTemplate(
		const FAssetDocumentTemplateRequest& Request,
		const FString& NormalizedTarget,
		const FString& AssetClassPath) override;

	FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override;

	IAssetDocumentAtomicTransaction* ResolveTransactionForPhase(
		const FAssetDocumentRequestContextMetadata& Metadata,
		IAssetDocumentAtomicTransaction* ActiveTransaction,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

private:
	// The request entry points above run these under FAssetDocumentEditorNonInteractiveScope.
	FAssetDocumentResult DispatchApplyFileImpl(const FAssetDocumentApplyFileRequest& Request);
	FAssetDocumentResult DispatchValidateImpl(const FAssetDocumentValidateRequest& Request);
	FAssetDocumentRuntimeResultDto DispatchExtractImpl(const FAssetDocumentExtractRequest& Request, const FString& AssetClassPath);
	FAssetDocumentRuntimeResultDto DispatchDiffImpl(const FAssetDocumentDiffRequest& Request);
	FAssetDocumentRuntimeResultDto DispatchInspectProfileImpl(const FAssetDocumentProfileRequest& Request, const FString& AssetClassPath);
	FAssetDocumentRuntimeResultDto DispatchInspectImpl(const FAssetDocumentInspectRequest& Request,
		const FString& AssetClassPath, const FString& AssetObjectPath);
	FAssetDocumentRuntimeResultDto DispatchCreateTemplateImpl(const FAssetDocumentTemplateRequest& Request,
		const FString& NormalizedTarget, const FString& AssetClassPath);

	TSharedPtr<FAssetDocumentAnimationRequestBinding> AnimationRequestBinding;
	TSharedPtr<FAssetDocumentStagedMontageReadBinding> StagedMontageReadBinding;
 TSharedPtr<FAssetDocumentStagedBehaviorTreeReadBinding> BehaviorTreeStagedReadBinding;
	TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> PreparationProjection;
 TSharedPtr<FAssetDocumentMutationComposition> ActiveMutationComposition;
 TMap<FString, FAssetDocumentRuntimeTargetActivation> TargetActivations;
	FAssetDocumentRuntimeValueDto RegistrationProjection;
	uint64 ActivationRevision = 0;
	bool bTargetActivationsBound = false;
	bool bCompositionEverBound = false;
	TOptional<FAssetDocumentPreparedTargetDecision> ActiveTargetDecision;
	IAssetDocumentPythonRuntime* Runtime = nullptr;
	FAssetDocumentCoreRuntimeComposition* Composition = nullptr;
	IAssetDocumentPythonRuntimeHost* ActiveRequestHost = nullptr;
	TOptional<FAssetDocumentBlackboardFootprintProjection>
		ActiveBlackboardProjection;
	TArray<FAssetDocumentMutationFootprint> ActiveBlackboardCreatePrefix;
	TUniquePtr<FAssetDocumentBlackboardAtomicTransactionAdapter>
		ActiveBlackboardAdapter;
};
