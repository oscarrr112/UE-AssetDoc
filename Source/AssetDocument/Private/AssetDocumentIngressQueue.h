// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "AssetDocumentTypes.h"

class FAssetDocumentService;

enum class EAssetDocumentIngressEventType : uint8
{
	ApplyFile,
	SidecarChanged,
	SidecarRescanRequested,
	AssetRenamedOrMoved,
	AssetAdded,
	AssetRemoved
};

struct FAssetDocumentIngressEvent
{
	EAssetDocumentIngressEventType Type =
		EAssetDocumentIngressEventType::ApplyFile;
	FAssetDocumentApplyFileRequest ApplyFileRequest;
	FString PrimaryPath;
	FString SecondaryPath;
};

class ASSETDOCUMENT_API FAssetDocumentIngressQueue final
	: public TSharedFromThis<FAssetDocumentIngressQueue>
{
public:
	explicit FAssetDocumentIngressQueue(
		const TSharedRef<FAssetDocumentService>& InService);
	~FAssetDocumentIngressQueue();
	FAssetDocumentIngressQueue(const FAssetDocumentIngressQueue&) = delete;
	FAssetDocumentIngressQueue& operator=(
		const FAssetDocumentIngressQueue&) = delete;

	FAssetDocumentResult SubmitApplyFile(
		const FAssetDocumentApplyFileRequest& Request);
	void Enqueue(FAssetDocumentIngressEvent Event);
	FGuid GetCoordinatorId() const;

private:
	struct FQueuedIngressEvent
	{
		FAssetDocumentIngressEvent Event;
		TSharedPtr<TPromise<FAssetDocumentResult>> Completion;
	};

	struct FDebouncedWatcherEvent
	{
		FQueuedIngressEvent QueuedEvent;
		FDateTime LastObservedUtc;
	};

	FAssetDocumentResult Submit(FAssetDocumentIngressEvent Event);
	void Admit(FQueuedIngressEvent Event, bool bScheduleDrain);
	void ScheduleDrain();
	void DrainOnGameThread();
	bool TickWatcherEvents(float DeltaSeconds);
	static bool IsWatcherEvent(
		EAssetDocumentIngressEventType EventType);
	static FString MakeWatcherEventKey(
		const FAssetDocumentIngressEvent& Event);

	TSharedPtr<FAssetDocumentService> Service;
	FGuid CoordinatorId;
	FTSTicker::FDelegateHandle WatcherTickerHandle;
	FCriticalSection PendingEventsLock;
	TArray<FQueuedIngressEvent> PendingEvents;
	TMap<FString, FDebouncedWatcherEvent> PendingWatcherEvents;
	bool bDrainScheduled = false;
	FTimespan WatcherDebounceInterval =
		FTimespan::FromMilliseconds(500);
};
