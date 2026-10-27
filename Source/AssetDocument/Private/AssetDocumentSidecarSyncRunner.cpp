// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentSidecarSyncRunner.h"

#include "AssetDocumentFileWatcher.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentPendingNotices.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSettings.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarPathUtils.h"
#include "AssetDocumentSidecarSyncState.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "AssetDocumentSidecarSyncRunner"

namespace SidecarSyncRunnerLocal
{
// Time a tick may spend on passes (at least one pass runs per tick).
constexpr double TickBudgetSeconds = 0.015;
int32 GFastPathCount = 0;
FCriticalSection GPublishedLock;
FString GPublishedStatus;
int32 GCollectionCount = 0;

FString ComparablePath(const FString& Path)
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Full);
	return Full;
}

// The outcomes after which an unchanged pair needs no new pass.
bool IsSettledState(const FString& State)
{
	return State == TEXT("in_sync") || State == TEXT("wrote") || State == TEXT("applied");
}

bool ShouldNotify()
{
	return FSlateApplication::IsInitialized() && !FApp::IsUnattended() && !GIsAutomationTesting;
}
}

FAssetDocumentSidecarSyncRunner::FAssetDocumentSidecarSyncRunner(FAssetDocumentService& InService)
	: Service(InService)
{
}

FAssetDocumentSidecarSyncRunner::~FAssetDocumentSidecarSyncRunner()
{
	Unregister();
}

void FAssetDocumentSidecarSyncRunner::Register()
{
	if (!SavedHandle.IsValid())
	{
		SavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FAssetDocumentSidecarSyncRunner::HandlePackageSaved);
	}
	if (!SettingsHandle.IsValid())
	{
		SettingsHandle = UAssetDocumentSettings::OnMirrorSettingsChanged().AddRaw(this, &FAssetDocumentSidecarSyncRunner::HandleSettingsChanged);
	}
	if (!TickHandle.IsValid())
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FAssetDocumentSidecarSyncRunner::Tick), 0.0f);
	}
	bWasEnabled = UE::AssetDocument::SidecarSync::IsMirroringEnabled();
	if (bWasEnabled)
	{
		RequestReconcile();
	}
}

void FAssetDocumentSidecarSyncRunner::Unregister()
{
	if (SavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(SavedHandle);
		SavedHandle.Reset();
	}
	if (SettingsHandle.IsValid())
	{
		UAssetDocumentSettings::OnMirrorSettingsChanged().Remove(SettingsHandle);
		SettingsHandle.Reset();
	}
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (const TSharedPtr<SNotificationItem> Item = Notification.Pin())
	{
		Item->ExpireAndFadeout();
	}
	Notification.Reset();
}

void FAssetDocumentSidecarSyncRunner::Enqueue(const FString& PackageName, EAssetDocumentSidecarSyncTrigger Trigger)
{
	const FString Package = UE::AssetDocument::SidecarSync::ToPackageName(PackageName);
	const FString Key = FAssetDocumentSidecarSyncIndex::MakeKey(Package);
	if (Package.IsEmpty() || QueuedKeys.Contains(Key))
	{
		return;
	}
	QueuedKeys.Add(Key);
	Queue.Add({Package, Trigger});
}

bool FAssetDocumentSidecarSyncRunner::IsQueued(const FString& PackageName) const
{
	return QueuedKeys.Contains(FAssetDocumentSidecarSyncIndex::MakeKey(PackageName));
}

bool FAssetDocumentSidecarSyncRunner::ShouldSyncSave(const FString& Filename, const UPackage* Package, bool bProceduralSave,
	bool bAutoSave, FString* OutWhy)
{
	using namespace SidecarSyncRunnerLocal;
	auto Skip = [OutWhy](const TCHAR* Why)
	{
		if (OutWhy)
		{
			*OutWhy = Why;
		}
		return false;
	};
	if (Package == nullptr)
	{
		return Skip(TEXT("no_package"));
	}
	if (bProceduralSave)
	{
		return Skip(TEXT("procedural"));
	}
	if (bAutoSave)
	{
		return Skip(TEXT("autosave"));
	}
	const FString PackageName = Package->GetName();
	if (FAssetDocumentSidecarSync::IsSyncing(PackageName))
	{
		return Skip(TEXT("syncing"));
	}
	FString OwnFile;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, OwnFile, FPackageName::GetAssetPackageExtension()))
	{
		return Skip(TEXT("not_a_content_package"));
	}
	const FString Saved = ComparablePath(Filename);
	const FString Own = ComparablePath(OwnFile);
	const FString OwnMap = ComparablePath(FPaths::ChangeExtension(OwnFile, FPackageName::GetMapPackageExtension()));
	if (!Saved.Equals(Own, ESearchCase::IgnoreCase) && !Saved.Equals(OwnMap, ESearchCase::IgnoreCase))
	{
		// A copy saved elsewhere (a staged or exported copy), not the package's own file.
		return Skip(TEXT("other_file"));
	}
	return true;
}

void FAssetDocumentSidecarSyncRunner::HandlePackageSaved(const FString& Filename, UPackage* Package, FObjectPostSaveContext Context)
{
	if (!UE::AssetDocument::SidecarSync::IsMirroringEnabled()
		|| !ShouldSyncSave(Filename, Package, Context.IsProceduralSave(), Context.IsFromAutoSave()))
	{
		return;
	}
	// Only queued here: the pass extracts, which must not happen inside the save.
	Enqueue(Package->GetName(), EAssetDocumentSidecarSyncTrigger::Save);
}

void FAssetDocumentSidecarSyncRunner::HandleSettingsChanged()
{
	const bool bEnabled = UE::AssetDocument::SidecarSync::IsMirroringEnabled();
	if (bEnabled)
	{
		// Turned on, or the scope changed: assets now in scope are reconciled and materialized.
		RequestReconcile();
	}
	else
	{
		Cancel();
		bReconcileRequested = false;
	}
	bWasEnabled = bEnabled;
}

void FAssetDocumentSidecarSyncRunner::RequestReconcile()
{
	bReconcileRequested = true;
}

bool FAssetDocumentSidecarSyncRunner::IsReadyForRun() const
{
	const IAssetRegistry* Registry = IAssetRegistry::Get();
	return Registry != nullptr && !Registry->IsLoadingAssets() && Service.IsSidecarSyncAvailable();
}

bool FAssetDocumentSidecarSyncRunner::IsUnchangedSinceLastSync(const FString& PackageName, const FString& SidecarPath)
{
	using namespace UE::AssetDocument::SidecarSync;
	FAssetDocumentSidecarSyncIndex& Index = Service.GetSidecarSync().GetIndex();
	const FAssetDocumentSidecarSyncIndexEntry* Entry = Index.Find(PackageName);
	if (Entry == nullptr || !SidecarSyncRunnerLocal::IsSettledState(Entry->State) || Entry->PackageHash.IsEmpty()
		|| Entry->PackageHash != GetPackageSavedHash(PackageName))
	{
		return false;
	}
	const FFileStatData Stat = IFileManager::Get().GetStatData(*SidecarPath);
	if (!Stat.bIsValid || Stat.FileSize != Entry->FileSize)
	{
		return false;
	}
	if (Stat.ModificationTime == Entry->FileTimestamp)
	{
		return true;
	}
	// Touched but maybe not changed (a checkout): the content decides.
	const FString Digest = DigestSidecarFile(SidecarPath);
	if (Digest.IsEmpty() || Digest == TEXT("unreadable") || Digest != Entry->FileDigest)
	{
		return false;
	}
	FAssetDocumentSidecarSyncIndexEntry Touched = *Entry;
	Touched.FileTimestamp = Stat.ModificationTime;
	Index.Set(PackageName, Touched);
	return true;
}

TArray<FString> FAssetDocumentSidecarSyncRunner::CollectPackagesUnder(const FString& Folder)
{
	using namespace UE::AssetDocument::SidecarSync;
	FString Root = Folder.TrimStartAndEnd();
	while (Root.Len() > 1 && Root.EndsWith(TEXT("/")))
	{
		Root.LeftChopInline(1);
	}
	TSet<FString> Seen;
	TArray<FString> Packages;
	auto Add = [&Seen, &Packages](const FString& Package)
	{
		if (!Package.IsEmpty() && IsInMirrorScope(Package) && !Seen.Contains(Package.ToLower()))
		{
			Seen.Add(Package.ToLower());
			Packages.Add(Package);
		}
	};
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Root));
	Filter.bRecursivePaths = true;
	Filter.bIncludeOnlyOnDiskAssets = false;
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (!Asset.IsRedirector())
		{
			Add(Asset.PackageName.ToString());
		}
	}
	FString Directory;
	if (FPackageName::TryConvertLongPackageNameToFilename(Root + TEXT("/"), Directory))
	{
		TArray<FString> Sidecars;
		FAssetDocumentFileWatcher::CollectAssetDocumentSidecarsUnderDirectory(Directory, Sidecars);
		for (const FString& Sidecar : Sidecars)
		{
			Add(ToPackageName(FAssetDocumentSidecar::ResolveObjectPathFromSidecar(Sidecar)));
		}
	}
	Packages.Sort();
	return Packages;
}

void FAssetDocumentSidecarSyncRunner::StartReconcile(const FString& Root)
{
	bReconcileRequested = false;
	if (IsRunActive())
	{
		Cancel();
	}
	Discovery = MakeUnique<FDiscovery>();
	Discovery->Root = Root;
	FString RootDirectory;
	if (FPackageName::TryConvertLongPackageNameToFilename(Root + TEXT("/"), RootDirectory))
	{
		RootDirectory = FAssetDocumentSidecarPathUtils::NormalizeFilePath(RootDirectory);
		FPaths::NormalizeDirectoryName(RootDirectory);
		if (IFileManager::Get().DirectoryExists(*RootDirectory))
		{
			Discovery->Directories.Add(RootDirectory);
		}
	}
	Run = FRunSummary();
	Run.Kind = TEXT("reconcile");
	SinceCollection = 0;
	UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument sidecar mirroring: reconcile of %s started"), *Root);
}

bool FAssetDocumentSidecarSyncRunner::StepDiscovery()
{
	using namespace UE::AssetDocument::SidecarSync;
	FDiscovery& Found = *Discovery;
	if (!Found.Directories.IsEmpty())
	{
		// One folder: its sidecars and its subfolders.
		const FString Directory = Found.Directories.Pop(EAllowShrinking::No);
		IFileManager::Get().IterateDirectory(*Directory, [&Found](const TCHAR* Path, bool bIsDirectory)
		{
			if (bIsDirectory)
			{
				Found.Directories.Add(Path);
			}
			else if (FAssetDocumentFileWatcher::IsAssetDocumentSidecarPath(Path))
			{
				Found.Sidecars.Add(FAssetDocumentSidecarPathUtils::NormalizeFilePath(Path));
			}
			return true;
		});
		return false;
	}
	if (Found.NextSidecar < Found.Sidecars.Num())
	{
		// An existing sidecar: a pass unless the index and the package say nothing changed.
		const FString& Sidecar = Found.Sidecars[Found.NextSidecar++];
		const FString PackageName = ToPackageName(FAssetDocumentSidecar::ResolveObjectPathFromSidecar(Sidecar));
		if (!PackageName.IsEmpty() && IsInMirrorScope(PackageName) && !Found.Seen.Contains(PackageName.ToLower()))
		{
			Found.Seen.Add(PackageName.ToLower());
			if (IsUnchangedSinceLastSync(PackageName, Sidecar))
			{
				++SidecarSyncRunnerLocal::GFastPathCount;
			}
			else
			{
				Found.Items.Add({PackageName, EAssetDocumentSidecarSyncTrigger::Reconcile});
			}
		}
		return false;
	}
	if (!Found.bAssetsListed)
	{
		Found.bAssetsListed = true;
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Found.Root));
		Filter.bRecursivePaths = true;
		Filter.bIncludeOnlyOnDiskAssets = false;
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (!Asset.IsRedirector())
			{
				Found.Assets.Add(Asset.PackageName.ToString());
			}
		}
		Found.Assets.Sort();
		return false;
	}
	if (Found.NextAsset < Found.Assets.Num())
	{
		// An asset without a sidecar: materialized, except one skipped before whose package did not change.
		const FString& PackageName = Found.Assets[Found.NextAsset++];
		if (!Found.Seen.Contains(PackageName.ToLower()) && IsInMirrorScope(PackageName))
		{
			Found.Seen.Add(PackageName.ToLower());
			const FAssetDocumentSidecarSyncIndexEntry* Entry = Service.GetSidecarSync().GetIndex().Find(PackageName);
			if (Entry == nullptr || Entry->State != TEXT("skipped") || Entry->PackageHash.IsEmpty()
				|| Entry->PackageHash != GetPackageSavedHash(PackageName))
			{
				Found.Items.Add({PackageName, EAssetDocumentSidecarSyncTrigger::Materialize});
			}
		}
		return false;
	}
	TArray<FQueued> Items = MoveTemp(Found.Items);
	Discovery.Reset();
	LastDiscoveredCount = Items.Num();
	BeginRun(TEXT("reconcile"), MoveTemp(Items));
	return true;
}

int32 FAssetDocumentSidecarSyncRunner::StartMaterialize(const TArray<FString>& PackageNames)
{
	TArray<FQueued> Items;
	TSet<FString> Seen;
	for (const FString& Name : PackageNames)
	{
		const FString Package = UE::AssetDocument::SidecarSync::ToPackageName(Name);
		if (!Package.IsEmpty() && !Seen.Contains(Package.ToLower()))
		{
			Seen.Add(Package.ToLower());
			Items.Add({Package, EAssetDocumentSidecarSyncTrigger::Materialize});
		}
	}
	const int32 Total = Items.Num();
	BeginRun(TEXT("materialize"), MoveTemp(Items));
	return Total;
}

void FAssetDocumentSidecarSyncRunner::BeginRun(const TCHAR* Kind, TArray<FQueued>&& Items)
{
	if (IsRunActive())
	{
		Cancel();
	}
	RunQueue = MoveTemp(Items);
	Run = FRunSummary();
	Run.Kind = Kind;
	Run.Total = RunQueue.Num();
	SinceCollection = 0;
	UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument sidecar mirroring: %s of %d assets"), Kind, Run.Total);
	if (RunQueue.IsEmpty())
	{
		FinishRun();
		return;
	}
	UpdateNotification(false);
}

void FAssetDocumentSidecarSyncRunner::Cancel()
{
	if (!IsRunActive())
	{
		return;
	}
	Discovery.Reset();
	RunQueue.Reset();
	Run.bCancelled = true;
	FinishRun();
}

void FAssetDocumentSidecarSyncRunner::FinishRun()
{
	if (SinceCollection > 0)
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		++SidecarSyncRunnerLocal::GCollectionCount;
		SinceCollection = 0;
	}
	FAssetDocumentSidecarSyncIndex& Index = Service.GetSidecarSync().GetIndex();
	if (Index.IsDirty())
	{
		Index.Save();
	}
	LastRun = Run;
	bHasLastRun = true;
	UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument sidecar mirroring: %s %s, %d of %d assets"), *Run.Kind,
		Run.bCancelled ? TEXT("cancelled") : TEXT("finished"), Run.Done, Run.Total);
	UpdateNotification(true);
	Run = FRunSummary();
}

void FAssetDocumentSidecarSyncRunner::UpdateNotification(bool bFinished)
{
	if (!SidecarSyncRunnerLocal::ShouldNotify())
	{
		return;
	}
	const FRunSummary& Shown = bFinished ? LastRun : Run;
	const FText Text = bFinished
		? FText::Format(LOCTEXT("Finished", "AssetDocument sidecar mirroring {0}: {1} of {2} assets"),
			Shown.bCancelled ? LOCTEXT("Cancelled", "cancelled") : LOCTEXT("Done", "done"),
			FText::AsNumber(Shown.Done), FText::AsNumber(Shown.Total))
		: FText::Format(LOCTEXT("Progress", "AssetDocument sidecar mirroring: {0} / {1} assets"),
			FText::AsNumber(Shown.Done), FText::AsNumber(Shown.Total));
	TSharedPtr<SNotificationItem> Item = Notification.Pin();
	if (!Item.IsValid() && !bFinished)
	{
		FNotificationInfo Info(Text);
		Info.bFireAndForget = false;
		Info.bUseThrobber = true;
		Info.FadeOutDuration = 1.0f;
		Info.ExpireDuration = 4.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("Cancel", "Cancel"),
			LOCTEXT("CancelTip", "Stop generating and syncing sidecars; the next editor start resumes."),
			FSimpleDelegate::CreateRaw(this, &FAssetDocumentSidecarSyncRunner::Cancel),
			SNotificationItem::CS_Pending));
		Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(SNotificationItem::CS_Pending);
		}
		Notification = Item;
		return;
	}
	if (!Item.IsValid())
	{
		return;
	}
	Item->SetText(Text);
	if (bFinished)
	{
		Item->SetCompletionState(LastRun.bCancelled ? SNotificationItem::CS_None : SNotificationItem::CS_Success);
		Item->ExpireAndFadeout();
		Notification.Reset();
	}
}

void FAssetDocumentSidecarSyncRunner::RunNextSave()
{
	const FQueued Item = Queue[0];
	Queue.RemoveAt(0);
	QueuedKeys.Remove(FAssetDocumentSidecarSyncIndex::MakeKey(Item.PackageName));
	if (Item.Trigger == EAssetDocumentSidecarSyncTrigger::SidecarChanged)
	{
		// A watcher event or a rescan: a file the last sync left as it is needs no pass.
		const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Item.PackageName);
		if (!SidecarPath.IsEmpty() && IFileManager::Get().FileExists(*SidecarPath) && IsUnchangedSinceLastSync(Item.PackageName, SidecarPath))
		{
			++SidecarSyncRunnerLocal::GFastPathCount;
			return;
		}
	}
	FAssetDocumentSidecarSyncRequest Request;
	Request.PackageName = Item.PackageName;
	Request.Trigger = Item.Trigger;
	const FAssetDocumentSidecarSyncOutcome Outcome = Service.GetSidecarSync().Sync(Request);
	UE_LOG(LogAssetDocument, Verbose, TEXT("AssetDocument sidecar sync of %s: %s"), *Outcome.PackageName, *Outcome.State);
	if (Item.Trigger == EAssetDocumentSidecarSyncTrigger::SidecarChanged && !Outcome.Apply.IsSet() && Outcome.State != TEXT("changed")
		&& FAssetDocumentSidecarSync::ToApplyResult(Outcome).IsSuccess())
	{
		// Nothing left to apply: an earlier failed watcher apply of this file is settled.
		FAssetDocumentPendingNotices::Resolve(FAssetDocumentService::WatcherApplyFailedNotice, Outcome.SidecarPath);
	}
	CollectAfterBatch();
}

void FAssetDocumentSidecarSyncRunner::CollectAfterBatch()
{
	if (++SinceCollection >= BatchSize)
	{
		// The packages this batch loaded are released before the next one loads more.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		++SidecarSyncRunnerLocal::GCollectionCount;
		SinceCollection = 0;
		Service.GetSidecarSync().GetIndex().Save();
	}
}

void FAssetDocumentSidecarSyncRunner::RunNextOfRun()
{
	const FQueued Item = RunQueue[0];
	RunQueue.RemoveAt(0);
	FAssetDocumentSidecarSyncRequest Request;
	Request.PackageName = Item.PackageName;
	Request.Trigger = Item.Trigger;
	const FAssetDocumentSidecarSyncOutcome Outcome = Service.GetSidecarSync().Sync(Request);
	++Run.Done;
	Run.States.FindOrAdd(Outcome.State)++;
	if (Outcome.State == TEXT("skipped"))
	{
		Run.Skipped.FindOrAdd(Outcome.Reason)++;
	}
	CollectAfterBatch();
	if (RunQueue.IsEmpty())
	{
		FinishRun();
	}
	else
	{
		UpdateNotification(false);
	}
}

void FAssetDocumentSidecarSyncRunner::PublishStatus() const
{
	FString Text;
	FJsonSerializer::Serialize(GetStatusJson(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	FScopeLock Lock(&SidecarSyncRunnerLocal::GPublishedLock);
	SidecarSyncRunnerLocal::GPublishedStatus = MoveTemp(Text);
}

TSharedPtr<FJsonObject> FAssetDocumentSidecarSyncRunner::GetPublishedStatus()
{
	FString Text;
	{
		FScopeLock Lock(&SidecarSyncRunnerLocal::GPublishedLock);
		Text = SidecarSyncRunnerLocal::GPublishedStatus;
	}
	TSharedPtr<FJsonObject> Status;
	if (Text.IsEmpty() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Status))
	{
		return nullptr;
	}
	return Status;
}

bool FAssetDocumentSidecarSyncRunner::Tick(float DeltaTime)
{
	ON_SCOPE_EXIT
	{
		PublishStatus();
	};
	if (IsGarbageCollecting() || UE::IsSavingPackage(nullptr) || IsAsyncLoading())
	{
		return true;
	}
	if (!UE::AssetDocument::SidecarSync::IsMirroringEnabled())
	{
		// Turned off: queued work is dropped.
		Queue.Reset();
		QueuedKeys.Reset();
		Cancel();
		return true;
	}
	if (bReconcileRequested && IsReadyForRun())
	{
		StartReconcile();
	}
	TickWithBudget(SidecarSyncRunnerLocal::TickBudgetSeconds);
	return true;
}

void FAssetDocumentSidecarSyncRunner::TickWithBudget(double BudgetSeconds)
{
	// Saves first, then the run (its search for work before its passes); at least one step per tick.
	const double Start = FPlatformTime::Seconds();
	while (!Queue.IsEmpty() || IsRunActive())
	{
		if (!Queue.IsEmpty())
		{
			RunNextSave();
		}
		else if (Discovery.IsValid())
		{
			StepDiscovery();
		}
		else
		{
			RunNextOfRun();
		}
		if (FPlatformTime::Seconds() - Start >= BudgetSeconds)
		{
			break;
		}
	}
}

void FAssetDocumentSidecarSyncRunner::TickForTest(double BudgetSeconds)
{
	TickWithBudget(BudgetSeconds);
	PublishStatus();
}

int32 FAssetDocumentSidecarSyncRunner::ReconcileNowForTest(const FString& Root)
{
	StartReconcile(Root);
	while (Discovery.IsValid())
	{
		StepDiscovery();
	}
	return LastDiscoveredCount;
}

int32 FAssetDocumentSidecarSyncRunner::DrainForTest()
{
	int32 Ran = 0;
	while (!Queue.IsEmpty() || IsRunActive())
	{
		if (!Queue.IsEmpty())
		{
			RunNextSave();
		}
		else if (Discovery.IsValid())
		{
			StepDiscovery();
			continue;
		}
		else
		{
			RunNextOfRun();
		}
		++Ran;
	}
	PublishStatus();
	return Ran;
}

int32 FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest()
{
	return SidecarSyncRunnerLocal::GFastPathCount;
}

int32 FAssetDocumentSidecarSyncRunner::GetCollectionCountForTest()
{
	return SidecarSyncRunnerLocal::GCollectionCount;
}

TSharedRef<FJsonObject> FAssetDocumentSidecarSyncRunner::FRunSummary::ToJson() const
{
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("kind"), Kind);
	Json->SetNumberField(TEXT("total"), Total);
	Json->SetNumberField(TEXT("done"), Done);
	Json->SetBoolField(TEXT("cancelled"), bCancelled);
	const TSharedRef<FJsonObject> StateCounts = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& State : States)
	{
		StateCounts->SetNumberField(State.Key, State.Value);
	}
	Json->SetObjectField(TEXT("states"), StateCounts);
	const TSharedRef<FJsonObject> SkippedCounts = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Reason : Skipped)
	{
		SkippedCounts->SetNumberField(Reason.Key, Reason.Value);
	}
	Json->SetObjectField(TEXT("skipped"), SkippedCounts);
	return Json;
}

TSharedRef<FJsonObject> FAssetDocumentSidecarSyncRunner::GetStatusJson() const
{
	const bool bEnabled = UE::AssetDocument::SidecarSync::IsMirroringEnabled();
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("enabled"), bEnabled);
	const TCHAR* State = !bEnabled ? TEXT("idle")
		: IsRunActive() ? (Run.Kind == TEXT("materialize") ? TEXT("materializing") : TEXT("reconciling"))
		: bReconcileRequested ? TEXT("waiting")
		: !Queue.IsEmpty() ? TEXT("syncing")
		: TEXT("idle");
	Json->SetStringField(TEXT("state"), State);
	Json->SetNumberField(TEXT("done"), RunQueue.IsEmpty() ? 0 : Run.Done);
	Json->SetNumberField(TEXT("total"), RunQueue.IsEmpty() ? 0 : Run.Total);
	if (Discovery.IsValid())
	{
		// Still finding the run's work: the total is not known yet.
		Json->SetBoolField(TEXT("discovering"), true);
	}
	Json->SetNumberField(TEXT("queued"), Queue.Num());
	if (bHasLastRun)
	{
		Json->SetObjectField(TEXT("last_run"), LastRun.ToJson());
	}
	return Json;
}

#undef LOCTEXT_NAMESPACE
