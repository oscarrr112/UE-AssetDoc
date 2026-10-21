// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

class IAssetDocumentRollbackParticipant
{
public:
	virtual ~IAssetDocumentRollbackParticipant() = default;
	virtual FStringView GetRollbackComponentId() const = 0;
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) = 0;
};

class IAssetDocumentRollbackParticipantRegistry
{
public:
	virtual ~IAssetDocumentRollbackParticipantRegistry() = default;
	virtual IAssetDocumentRollbackParticipant* Resolve(FStringView RollbackComponentId) = 0;
};

using FAssetDocumentAtomicDiagnosticSink =
	TFunction<void(EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic&)>;

class FAssetDocumentAtomicTransactionRuntime final : public IAssetDocumentAtomicTransaction
{
public:
	FAssetDocumentAtomicTransactionRuntime(
		const FAssetDocumentPreflightReceipt& Receipt,
		IAssetDocumentHandleResolver& InHandleResolver,
		IAssetDocumentRollbackParticipantRegistry& InParticipantRegistry,
		FAssetDocumentAtomicDiagnosticSink InDiagnosticSink,
		TArray<TSharedRef<IAssetDocumentRollbackAction>> InInitialActions = {});
	virtual ~FAssetDocumentAtomicTransactionRuntime() override;

	virtual FGuid GetTransactionId() const override;
	virtual EAssetDocumentAtomicState GetState() const override;
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override;
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override;
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override;
	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt& Receipt) override;
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override;
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override;
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override;

private:
	struct FRollbackTarget
	{
		FRollbackTarget(
			FString InComponentId,
			TSharedRef<IAssetDocumentRollbackAction> InAction);
		FRollbackTarget(
			FString InComponentId,
			IAssetDocumentRollbackParticipant* InParticipant);

		FAssetDocumentContractValidationResult Rollback(
			FStringView StableFailureCode) const;

		FString ComponentId;
		TVariant<
			TSharedRef<IAssetDocumentRollbackAction>,
			IAssetDocumentRollbackParticipant*> Target;
	};

	FAssetDocumentContractValidationResult Reject(
		EAssetDocumentRuntimeRequestPhase Phase,
		const TCHAR* Code,
		const TCHAR* Message) const;
	void Invalidate(EAssetDocumentHandleInvalidationReason Reason);
	void ReleaseRollbackActionOwnership();
	FAssetDocumentContractValidationResult RollbackLive(FStringView StableFailureCode);
	FAssetDocumentContractValidationResult ValidateEnlistmentRequest(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) const;
	FAssetDocumentContractValidationResult ValidateEnlistmentItems(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) const;
	FAssetDocumentContractValidationResult ResolveRollbackTargets(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint,
		TArray<FRollbackTarget>& OutTargets) const;
	void EnlistItem(const FAssetDocumentMutationFootprint& Item);
	bool IsMutableState() const;
	bool HasDeclaredComponent(FStringView ComponentId) const;
	bool HasMatchingReceiptIdentity(const FAssetDocumentDurableCompletionReceipt& Receipt) const;
	bool HasDurableReceiptPayload(const FAssetDocumentDurableCompletionReceipt& Receipt) const;
	const FAssetDocumentMutationFootprint* FindDeclared(FStringView StableId) const;
	bool IsTerminal() const;

	FAssetDocumentPreflightReceipt PreflightReceipt;
	IAssetDocumentHandleResolver& HandleResolver;
	IAssetDocumentRollbackParticipantRegistry& ParticipantRegistry;
	FAssetDocumentAtomicDiagnosticSink DiagnosticSink;
	FAssetDocumentMutationToken MutationToken;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TSet<FString> EnlistedStableIds;
	TSet<FString> EnlistedComponentIds;
	TMap<FString, TSharedRef<IAssetDocumentRollbackAction>> RollbackActions;
	// The constructor's actions (the editor's rollback seeds): the outermost rollback layer.
	TSet<FString> InitialComponentIds;
	TArray<FRollbackTarget> RollbackTargets;
	TSharedPtr<IAssetDocumentPostRollbackFinalizer> PostRollbackFinalizer;
	FString PostRollbackFinalizerComponentId;
	bool bPostRollbackFinalizerActive = false;
};

class FAssetDocumentAtomicTransactionFactoryRuntime final : public IAssetDocumentAtomicTransactionFactory
{
public:
	FAssetDocumentAtomicTransactionFactoryRuntime(
		IAssetDocumentHandleResolver& InHandleResolver,
		IAssetDocumentRollbackParticipantRegistry& InParticipantRegistry,
		FAssetDocumentAtomicDiagnosticSink InDiagnosticSink = {},
		TArray<TSharedRef<IAssetDocumentRollbackAction>> InInitialActions = {});

	virtual TUniquePtr<IAssetDocumentAtomicTransaction> BeginLiveAfterPreflight(
		const FAssetDocumentPreflightReceipt& Receipt,
		IAssetDocumentCancellationView& Cancellation) override;

private:
	TUniquePtr<IAssetDocumentAtomicTransaction> RejectBegin(
		const FAssetDocumentContractValidationResult& Validation) const;
	IAssetDocumentHandleResolver& HandleResolver;
	IAssetDocumentRollbackParticipantRegistry& ParticipantRegistry;
	FAssetDocumentAtomicDiagnosticSink DiagnosticSink;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> InitialActions;
	FAssetDocumentContractValidationResult InitialActionsValidation;
};
