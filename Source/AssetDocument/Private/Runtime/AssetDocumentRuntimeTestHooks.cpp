// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentRuntimeTestHooks.h"

#include "AssetDocumentAtomicFile.h"
#include "Runtime/AssetDocumentPersistenceJournal.h"

namespace
{
TOptional<EAssetDocumentRuntimeRequestPhase> GArmedRequestPhase;
FString GArmedPersistenceEvent;
TFunction<void(FStringView)> GPersistenceEventObserver;
}

void FAssetDocumentRuntimeTestHooks::SetPersistenceEventObserver(TFunction<void(FStringView Event)> Observer)
{
	check(IsInGameThread());
	GPersistenceEventObserver = MoveTemp(Observer);
}

void FAssetDocumentRuntimeTestHooks::FailNextRequestAtPhase(EAssetDocumentRuntimeRequestPhase Phase)
{
	check(IsInGameThread());
	GArmedRequestPhase = Phase;
}

void FAssetDocumentRuntimeTestHooks::FailNextPersistenceAtEvent(const FString& Event)
{
	check(IsInGameThread());
	GArmedPersistenceEvent = Event;
}

void FAssetDocumentRuntimeTestHooks::Reset()
{
	GArmedRequestPhase.Reset();
	GArmedPersistenceEvent.Reset();
	GPersistenceEventObserver = nullptr;
	FAssetDocumentAtomicFile::ResetFailureForTest();
}

bool FAssetDocumentRuntimeTestHooks::ConsumeRequestPhaseFailure(EAssetDocumentRuntimeRequestPhase Phase)
{
	if (!GArmedRequestPhase.IsSet() || GArmedRequestPhase.GetValue() != Phase || !IsInGameThread())
	{
		return false;
	}
	GArmedRequestPhase.Reset();
	return true;
}

void FAssetDocumentRuntimeTestHooks::ObservePersistenceEvent(FStringView Event)
{
	if (GPersistenceEventObserver && IsInGameThread())
	{
		GPersistenceEventObserver(Event);
	}
	if (GArmedPersistenceEvent.IsEmpty() || Event != GArmedPersistenceEvent || !IsInGameThread())
	{
		return;
	}
	GArmedPersistenceEvent.Reset();
	if (Event == TEXT("Cleanup"))
	{
		FAssetDocumentPersistenceJournal::FailNextCleanupForTest();
	}
	else
	{
		FAssetDocumentAtomicFile::FailNextWriteAtForTest(EAssetDocumentAtomicFileFailurePoint::Flush);
	}
}
