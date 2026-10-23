// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentRollbackAction.h"

#include "Runtime/AssetDocumentAtomicTransactionRuntime.h"

namespace
{
FAssetDocumentContractValidationResult MakeCompositionResult(
	bool bValid,
	const TCHAR* Code = TEXT(""),
	const TCHAR* Message = TEXT(""))
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/RollbackActionComposition");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

class FActionRollbackParticipant final
	: public IAssetDocumentRollbackParticipant
{
public:
	FActionRollbackParticipant(
		FString InComponentId,
		IAssetDocumentRollbackAction& InAction)
		: ComponentId(MoveTemp(InComponentId))
		, Action(InAction)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		return Action.Rollback(StableFailureCode);
	}

private:
	FString ComponentId;
	IAssetDocumentRollbackAction& Action;
};

class FActionRollbackParticipantRegistry final
	: public IAssetDocumentRollbackParticipantRegistry
{
public:
	FActionRollbackParticipantRegistry(
		TConstArrayView<TSharedRef<IAssetDocumentRollbackAction>> Actions,
		FAssetDocumentContractValidationResult& OutValidation)
	{
		OutValidation = MakeCompositionResult(true);
		for (const TSharedRef<IAssetDocumentRollbackAction>& Action : Actions)
		{
			FString ComponentId(Action->GetRollbackComponentId());
			if (ComponentId.IsEmpty())
			{
				OutValidation = MakeCompositionResult(
					false,
					TEXT("RollbackActionComposition.EmptyComponentId"),
					TEXT("Rollback action component IDs must not be empty."));
				return;
			}
			if (ParticipantsById.Contains(ComponentId))
			{
				OutValidation = MakeCompositionResult(
					false,
					TEXT("RollbackActionComposition.DuplicateComponentId"),
					TEXT("Rollback action component IDs must be unique."));
				return;
			}
			TUniquePtr<FActionRollbackParticipant> Participant =
				MakeUnique<FActionRollbackParticipant>(
					ComponentId, Action.Get());
			ParticipantsById.Add(ComponentId, Participant.Get());
			Participants.Add(MoveTemp(Participant));
		}
	}

	virtual IAssetDocumentRollbackParticipant* Resolve(
		FStringView RollbackComponentId) override
	{
		IAssetDocumentRollbackParticipant** Participant =
			ParticipantsById.Find(FString(RollbackComponentId));
		return Participant != nullptr ? *Participant : nullptr;
	}

private:
	TArray<TUniquePtr<FActionRollbackParticipant>> Participants;
	TMap<FString, IAssetDocumentRollbackParticipant*> ParticipantsById;
};
}

struct FAssetDocumentRollbackActionComposition::FImpl
{
	FImpl(
		IAssetDocumentHandleResolver& InHandleResolver,
		TArray<TSharedRef<IAssetDocumentRollbackAction>> InActions,
		FAssetDocumentAtomicDiagnosticSink InDiagnosticSink)
		: Actions(MoveTemp(InActions))
		, DiagnosticSink(MoveTemp(InDiagnosticSink))
		, Registry(Actions, Validation)
		, Factory(
			InHandleResolver,
			Registry,
			DiagnosticSink,
			Actions)
	{
	}

	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	FAssetDocumentAtomicDiagnosticSink DiagnosticSink;
	FAssetDocumentContractValidationResult Validation;
	FActionRollbackParticipantRegistry Registry;
	FAssetDocumentAtomicTransactionFactoryRuntime Factory;
};

FAssetDocumentRollbackActionComposition::
	FAssetDocumentRollbackActionComposition(
		IAssetDocumentHandleResolver& InHandleResolver,
		TArray<TSharedRef<IAssetDocumentRollbackAction>> InActions,
		TFunction<void(
			EAssetDocumentRuntimeRequestPhase,
			const FAssetDocumentRuntimeDiagnostic&)> InDiagnosticSink)
	: Impl(MakeUnique<FImpl>(
		InHandleResolver, MoveTemp(InActions), MoveTemp(InDiagnosticSink)))
{
}

FAssetDocumentRollbackActionComposition::
	~FAssetDocumentRollbackActionComposition() = default;

FAssetDocumentRollbackActionComposition::
	FAssetDocumentRollbackActionComposition(
		FAssetDocumentRollbackActionComposition&& Other) = default;

FAssetDocumentRollbackActionComposition&
FAssetDocumentRollbackActionComposition::operator=(
	FAssetDocumentRollbackActionComposition&& Other) = default;

bool FAssetDocumentRollbackActionComposition::IsValid() const
{
	return Impl && Impl->Validation.bValid;
}

const FAssetDocumentContractValidationResult&
FAssetDocumentRollbackActionComposition::GetValidation() const
{
	return Impl->Validation;
}

TUniquePtr<IAssetDocumentAtomicTransaction>
FAssetDocumentRollbackActionComposition::BeginLiveAfterPreflight(
	const FAssetDocumentPreflightReceipt& Receipt,
	IAssetDocumentCancellationView& Cancellation)
{
	if (!Impl->Validation.bValid)
	{
		if (Impl->DiagnosticSink)
		{
			Impl->DiagnosticSink(
				EAssetDocumentRuntimeRequestPhase::Preflight,
				Impl->Validation.Diagnostics[0]);
		}
		return nullptr;
	}
	return Impl->Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
}
