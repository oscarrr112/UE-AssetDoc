// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentIngressQueue.h"

#include "AssetDocumentService.h"

#include "Async/Async.h"
#include "Misc/ScopeLock.h"

FAssetDocumentIngressQueue::FAssetDocumentIngressQueue(
	const TSharedRef<FAssetDocumentService>& InService)
	: Service(InService)
	, CoordinatorId(FGuid::NewGuid())
{
	WatcherTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(
			this,
			&FAssetDocumentIngressQueue::TickWatcherEvents),
		0.25f);
}

FAssetDocumentIngressQueue::~FAssetDocumentIngressQueue()
{
	if (WatcherTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(WatcherTickerHandle);
		WatcherTickerHandle.Reset();
	}
}

FAssetDocumentResult FAssetDocumentIngressQueue::SubmitApplyFile(
	const FAssetDocumentApplyFileRequest& Request)
{
	FAssetDocumentIngressEvent Event;
	Event.Type = EAssetDocumentIngressEventType::ApplyFile;
	Event.ApplyFileRequest = Request;
	return Submit(MoveTemp(Event));
}

void FAssetDocumentIngressQueue::Enqueue(
	FAssetDocumentIngressEvent Event)
{
	FQueuedIngressEvent QueuedEvent;
	QueuedEvent.Event = MoveTemp(Event);

	if (IsWatcherEvent(QueuedEvent.Event.Type))
	{
		const FString EventKey =
			MakeWatcherEventKey(QueuedEvent.Event);
		FScopeLock Lock(&PendingEventsLock);
		FDebouncedWatcherEvent& Pending =
			PendingWatcherEvents.FindOrAdd(EventKey);
		Pending.QueuedEvent = MoveTemp(QueuedEvent);
		Pending.LastObservedUtc = FDateTime::UtcNow();
		return;
	}

	Admit(MoveTemp(QueuedEvent), true);
}

FAssetDocumentResult FAssetDocumentIngressQueue::Submit(
	FAssetDocumentIngressEvent Event)
{
	TSharedRef<TPromise<FAssetDocumentResult>> Completion =
		MakeShared<TPromise<FAssetDocumentResult>>();
	TFuture<FAssetDocumentResult> Future = Completion->GetFuture();

	FQueuedIngressEvent QueuedEvent;
	QueuedEvent.Event = MoveTemp(Event);
	QueuedEvent.Completion = Completion;
	const bool bNeedsScheduledDrain = !IsInGameThread();
	Admit(MoveTemp(QueuedEvent), bNeedsScheduledDrain);
	if (!bNeedsScheduledDrain)
	{
		DrainOnGameThread();
	}
	return Future.Get();
}

void FAssetDocumentIngressQueue::Admit(
	FQueuedIngressEvent Event,
	bool bScheduleDrain)
{
	bool bShouldSchedule = false;
	{
		FScopeLock Lock(&PendingEventsLock);
		PendingEvents.Add(MoveTemp(Event));
		if (bScheduleDrain && !bDrainScheduled)
		{
			bDrainScheduled = true;
			bShouldSchedule = true;
		}
	}

	if (bShouldSchedule)
	{
		ScheduleDrain();
	}
}

void FAssetDocumentIngressQueue::ScheduleDrain()
{
	const TWeakPtr<FAssetDocumentIngressQueue> WeakQueue = AsShared();
	AsyncTask(ENamedThreads::GameThread, [WeakQueue]()
	{
		if (const TSharedPtr<FAssetDocumentIngressQueue> Pinned =
			WeakQueue.Pin())
		{
			Pinned->DrainOnGameThread();
		}
	});
}

void FAssetDocumentIngressQueue::DrainOnGameThread()
{
	check(IsInGameThread());
	TArray<FQueuedIngressEvent> Events;
	{
		FScopeLock Lock(&PendingEventsLock);
		Events = MoveTemp(PendingEvents);
		PendingEvents.Reset();
		bDrainScheduled = false;
	}

	for (FQueuedIngressEvent& QueuedEvent : Events)
	{
		const FAssetDocumentResult Result = Service.IsValid()
			? Service->HandleIngressEvent(QueuedEvent.Event)
			: FAssetDocumentResult::Failure(
				TEXT("AssetDocument ingress service is unavailable"));
		if (QueuedEvent.Completion.IsValid())
		{
			QueuedEvent.Completion->SetValue(Result);
		}
	}
}

bool FAssetDocumentIngressQueue::TickWatcherEvents(float DeltaSeconds)
{
	(void)DeltaSeconds;
	check(IsInGameThread());

	bool bHasReadyEvents = false;
	const FDateTime Now = FDateTime::UtcNow();
	{
		FScopeLock Lock(&PendingEventsLock);
		for (TMap<FString, FDebouncedWatcherEvent>::TIterator It(
			PendingWatcherEvents);
			It;
			++It)
		{
			if (Now - It.Value().LastObservedUtc
				< WatcherDebounceInterval)
			{
				continue;
			}

			PendingEvents.Add(
				MoveTemp(It.Value().QueuedEvent));
			It.RemoveCurrent();
			bHasReadyEvents = true;
		}
	}

	if (bHasReadyEvents)
	{
		DrainOnGameThread();
	}
	return true;
}

bool FAssetDocumentIngressQueue::IsWatcherEvent(
	EAssetDocumentIngressEventType EventType)
{
	return EventType
			== EAssetDocumentIngressEventType::SidecarChanged
		|| EventType
			== EAssetDocumentIngressEventType::SidecarRescanRequested;
}

FString FAssetDocumentIngressQueue::MakeWatcherEventKey(
	const FAssetDocumentIngressEvent& Event)
{
	return FString::Printf(
		TEXT("%d:%s"),
		static_cast<int32>(Event.Type),
		*Event.PrimaryPath);
}

FGuid FAssetDocumentIngressQueue::GetCoordinatorId() const
{
	return CoordinatorId;
}
