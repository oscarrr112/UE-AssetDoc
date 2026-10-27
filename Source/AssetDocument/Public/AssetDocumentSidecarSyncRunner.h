// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentSidecarSync.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "UObject/ObjectSaveContext.h"

class FAssetDocumentService;
class SNotificationItem;
class UPackage;

// Runs sidecar sync passes on the game thread, a few per tick, for work nobody waits on. Only while mirroring is on
// (IsMirroringEnabled).
//   - Package saves, and with mirroring on the file watcher's changed sidecars and rescans: one queued pass per
//     package, however often it changes before the pass runs. They run first; a changed sidecar whose file and
//     package are as the last sync left them needs no pass.
//   - Reconcile, at editor start (once the Asset Registry has scanned and the Python runtime is up) and when mirroring
//     is turned on: every existing sidecar whose file or package changed since the last sync gets a pass (the local
//     index and _meta.sync.Package decide without Python or an extract), then every asset in scope without a sidecar
//     is materialized. Assets whose mirror was skipped (empty, failed, too large) are not retried until their package
//     changes. Finding that work (listing folders, checking each sidecar and asset) is itself done a step at a time
//     within the tick budget.
//   - Materialize (sync_asset_document): the named assets, skipped ones included.
// A reconcile or materialize run shows a progress notification with Cancel, reports done/total in GetStatusJson
// (health_check), and collects garbage after every batch, so the packages its passes loaded are released.
class ASSETDOCUMENT_API FAssetDocumentSidecarSyncRunner
{
public:
	static constexpr int32 BatchSize = 32;

	explicit FAssetDocumentSidecarSyncRunner(FAssetDocumentService& InService);
	~FAssetDocumentSidecarSyncRunner();

	// Subscribes to package saves and mirroring setting changes, starts ticking, and schedules the start reconcile
	// when mirroring is on.
	void Register();
	void Unregister();

	// Queues a save pass for the package (an object path is reduced to its package). A package already queued keeps
	// its place.
	void Enqueue(const FString& PackageName, EAssetDocumentSidecarSyncTrigger Trigger);
	bool IsQueued(const FString& PackageName) const;
	int32 GetQueuedCount() const { return Queue.Num(); }

	// Whether a save of Package to Filename starts a pass: a save of the package to its own file, not a procedural
	// (cook) save or an autosave, and not a save made by a running pass. OutWhy names the reason when it does not.
	static bool ShouldSyncSave(const FString& Filename, const UPackage* Package, bool bProceduralSave, bool bAutoSave,
		FString* OutWhy = nullptr);

	// The reconcile run (see above) starts at the next tick at which the Asset Registry and the Python runtime are
	// ready.
	void RequestReconcile();
	// Starts the reconcile run now, of the sidecars and assets under Root (a /Game path): the ticks that follow find
	// the work, then run its passes.
	void StartReconcile(const FString& Root = TEXT("/Game"));
	// Starts a materialize run of these packages (replacing a run in progress); returns how many were queued.
	int32 StartMaterialize(const TArray<FString>& PackageNames);
	// Stops the run in progress (save passes still run).
	void Cancel();
	bool IsRunActive() const { return !RunQueue.IsEmpty() || Discovery.IsValid(); }

	// {"enabled", "state": idle | waiting | syncing | reconciling | materializing, "done", "total", "queued",
	//  "last_run": {"kind", "total", "done", "cancelled", "states": {...}, "skipped": {...}}}
	TSharedRef<FJsonObject> GetStatusJson() const;

	// GetStatusJson as of the runner's last tick, readable from any thread (health_check).
	static TSharedPtr<FJsonObject> GetPublishedStatus();
	void PublishStatus() const;

	// The /Game packages in scope under Folder (a /Game path), and the packages of the sidecars there.
	static TArray<FString> CollectPackagesUnder(const FString& Folder);

	// Whether the existing sidecar needs no pass: unchanged since the last sync, and so is its package.
	bool IsUnchangedSinceLastSync(const FString& PackageName, const FString& SidecarPath);

	// Runs every queued pass now (saves and the run, the reconcile's search for work first) and returns how many ran.
	int32 DrainForTest();
	// One tick with this much time for work.
	void TickForTest(double BudgetSeconds);
	// StartReconcile, then its search for work at once; returns the number of passes it queued.
	int32 ReconcileNowForTest(const FString& Root);
	// Reconcile passes the index answered without Python or an extract, and garbage collections, since start.
	static int32 GetFastPathCountForTest();
	static int32 GetCollectionCountForTest();

private:
	struct FQueued
	{
		FString PackageName;
		EAssetDocumentSidecarSyncTrigger Trigger = EAssetDocumentSidecarSyncTrigger::Save;
	};
	struct FRunSummary
	{
		FString Kind;
		int32 Total = 0;
		int32 Done = 0;
		bool bCancelled = false;
		TMap<FString, int32> States;
		TMap<FString, int32> Skipped;
		TSharedRef<FJsonObject> ToJson() const;
	};

	void HandlePackageSaved(const FString& Filename, UPackage* Package, FObjectPostSaveContext Context);
	void HandleSettingsChanged();
	bool Tick(float DeltaTime);
	void TickWithBudget(double BudgetSeconds);
	// One step of the reconcile's search for work (a folder listed, a sidecar or an asset checked); true when the
	// search is over and its passes were queued as the run.
	bool StepDiscovery();
	bool IsReadyForRun() const;
	void RunNextSave();
	void RunNextOfRun();
	// Collects garbage (and saves the index) after every BatchSize passes, queued or of a run.
	void CollectAfterBatch();
	void BeginRun(const TCHAR* Kind, TArray<FQueued>&& Items);
	void FinishRun();
	void UpdateNotification(bool bFinished);

	// The reconcile's search for work, done a step at a time.
	struct FDiscovery
	{
		FString Root;
		TArray<FString> Directories;
		TArray<FString> Sidecars;
		int32 NextSidecar = 0;
		bool bAssetsListed = false;
		TArray<FString> Assets;
		int32 NextAsset = 0;
		TSet<FString> Seen;
		TArray<FQueued> Items;
	};

	FAssetDocumentService& Service;
	TUniquePtr<FDiscovery> Discovery;
	int32 LastDiscoveredCount = 0;
	TArray<FQueued> Queue;
	TSet<FString> QueuedKeys;
	TArray<FQueued> RunQueue;
	FRunSummary Run;
	FRunSummary LastRun;
	bool bHasLastRun = false;
	bool bReconcileRequested = false;
	bool bWasEnabled = false;
	int32 SinceCollection = 0;
	TWeakPtr<SNotificationItem> Notification;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle SavedHandle;
	FDelegateHandle SettingsHandle;
};
