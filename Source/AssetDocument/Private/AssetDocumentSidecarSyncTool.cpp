// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentService.h"

#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarSync.h"
#include "AssetDocumentSidecarSyncRunner.h"
#include "AssetDocumentSidecarSyncState.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace SidecarSyncToolLocal
{
// Assets one status, accept or materialize call takes (folders included once expanded).
constexpr int32 MaxCheckedTargets = 500;
constexpr int32 MaxIssues = 200;

FAssetDocumentResult ToolFailure(const TCHAR* Code, const FString& Message)
{
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = TEXT("error");
	return FAssetDocumentResult::FailureFromDiagnostics({Diagnostic}, FString::Printf(TEXT("%s: %s"), Code, *Message));
}

bool IsFolder(const FString& Target)
{
	if (Target == TEXT("/Game") || Target.EndsWith(TEXT("/")))
	{
		return true;
	}
	if (Target.Contains(TEXT(".")) || !FPackageName::IsValidLongPackageName(Target))
	{
		return false;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	return Registry.PathExists(FName(*Target)) && !FPackageName::DoesPackageExist(Target);
}

// The packages of the targets (folders expanded); false with OutError when a target is not under /Game.
bool ExpandTargets(const TArray<FString>& Targets, bool bAllowFolders, TArray<FString>& OutPackages, FString& OutError)
{
	using namespace UE::AssetDocument::SidecarSync;
	TSet<FString> Seen;
	for (const FString& Raw : Targets)
	{
		const FString Target = Raw.TrimStartAndEnd();
		if (Target != TEXT("/Game") && !Target.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("'%s' is not a /Game asset or folder"), *Target);
			return false;
		}
		TArray<FString> Packages;
		if (IsFolder(Target) || Target == TEXT("/Game"))
		{
			if (!bAllowFolders)
			{
				OutError = FString::Printf(TEXT("'%s' is a folder; name the assets to resolve"), *Target);
				return false;
			}
			Packages = FAssetDocumentSidecarSyncRunner::CollectPackagesUnder(Target);
		}
		else
		{
			Packages.Add(ToPackageName(Target));
		}
		for (const FString& Package : Packages)
		{
			if (!Seen.Contains(Package.ToLower()))
			{
				Seen.Add(Package.ToLower());
				OutPackages.Add(Package);
			}
		}
	}
	return true;
}

// The status of one asset without a pass, when that is known from the files and the sync index alone: mirroring
// off, no sidecar (unmirrored, or a skip the package did not outdate), or a pair unchanged since its last sync.
TSharedPtr<FJsonObject> QuickStatusOf(FAssetDocumentSidecarSync& Sync, FAssetDocumentSidecarSyncRunner& Runner,
	const FString& PackageName, bool bEnabled)
{
	using namespace UE::AssetDocument::SidecarSync;
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("asset"), PackageName);
	FString Reason;
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(PackageName);
	if (SidecarPath.IsEmpty() || !IsInMirrorScope(PackageName, &Reason))
	{
		Json->SetStringField(TEXT("state"), TEXT("excluded"));
		Json->SetStringField(TEXT("reason"), Reason.IsEmpty() ? FString(TEXT("not_game")) : Reason);
		return Json;
	}
	const bool bSidecar = !SidecarPath.IsEmpty() && IFileManager::Get().FileExists(*SidecarPath);
	if (bSidecar)
	{
		Json->SetStringField(TEXT("sidecar"), SidecarPath);
	}
	if (!bEnabled)
	{
		Json->SetStringField(TEXT("state"), TEXT("off"));
		return Json;
	}
	// An asset with unsaved changes, or one that does not exist, is left to the pass, which reports it without
	// loading anything.
	const UPackage* LoadedPackage = FindPackage(nullptr, *PackageName);
	if ((LoadedPackage != nullptr && LoadedPackage->IsDirty())
		|| (LoadedPackage == nullptr && !FPackageName::DoesPackageExist(PackageName)))
	{
		return nullptr;
	}
	const FAssetDocumentSidecarSyncIndexEntry* Entry = Sync.GetIndex().Find(PackageName);
	if (!bSidecar)
	{
		const bool bSkipped = Entry != nullptr && Entry->State == TEXT("skipped") && !Entry->PackageHash.IsEmpty()
			&& Entry->PackageHash == GetPackageSavedHash(PackageName);
		Json->SetStringField(TEXT("state"), bSkipped ? TEXT("skipped") : TEXT("unmirrored"));
		if (bSkipped && !Entry->Reason.IsEmpty())
		{
			Json->SetStringField(TEXT("reason"), Entry->Reason);
		}
		return Json;
	}
	if (Runner.IsUnchangedSinceLastSync(PackageName, SidecarPath))
	{
		Json->SetStringField(TEXT("state"), TEXT("in_sync"));
		return Json;
	}
	return nullptr;
}

// The status of one asset from a dry run of its pass.
TSharedRef<FJsonObject> StatusOf(FAssetDocumentSidecarSync& Sync, const FString& PackageName)
{
	FAssetDocumentSidecarSyncRequest Request;
	Request.PackageName = PackageName;
	Request.Trigger = EAssetDocumentSidecarSyncTrigger::Reconcile;
	Request.bDryRun = true;
	const FAssetDocumentSidecarSyncOutcome Outcome = Sync.Sync(Request);
	const TSharedRef<FJsonObject> Json = Outcome.ToJson();
	const bool bSidecar = !Outcome.SidecarPath.IsEmpty() && IFileManager::Get().FileExists(*Outcome.SidecarPath);
	FString State = Outcome.State;
	if (State == TEXT("would_apply"))
	{
		State = bSidecar ? TEXT("sidecar_changed") : TEXT("unmirrored");
	}
	else if (State == TEXT("would_write"))
	{
		State = bSidecar ? TEXT("asset_changed") : TEXT("unmirrored");
	}
	Json->SetStringField(TEXT("state"), State);
	return Json;
}
}

FAssetDocumentResult FAssetDocumentService::SyncSidecars(const FString& Action, const TArray<FString>& Targets, const TArray<FString>& Regions)
{
	using namespace SidecarSyncToolLocal;
	using namespace UE::AssetDocument::SidecarSync;
	const bool bEnabled = IsMirroringEnabled();
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("action"), Action);
	Payload->SetObjectField(TEXT("runner"), SidecarSyncRunner->GetStatusJson());

	if (Action == TEXT("status"))
	{
		if (Targets.IsEmpty())
		{
			// Project counts and every asset with an open issue, from the records of earlier passes.
			TMap<FString, int32> Counts;
			TArray<TSharedPtr<FJsonValue>> Issues;
			for (const TPair<FString, FAssetDocumentSidecarSyncIndexEntry>& Entry : SidecarSync->GetIndex().GetEntries())
			{
				Counts.FindOrAdd(Entry.Value.State)++;
				const bool bIssue = Entry.Value.State != TEXT("in_sync") && Entry.Value.State != TEXT("wrote")
					&& Entry.Value.State != TEXT("applied");
				if (bIssue && Issues.Num() < MaxIssues)
				{
					const TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("asset"), Entry.Key);
					Issue->SetStringField(TEXT("state"), Entry.Value.State);
					if (!Entry.Value.Reason.IsEmpty())
					{
						Issue->SetStringField(TEXT("reason"), Entry.Value.Reason);
					}
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}
			const TSharedRef<FJsonObject> CountJson = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& Count : Counts)
			{
				CountJson->SetNumberField(Count.Key, Count.Value);
			}
			Payload->SetObjectField(TEXT("counts"), CountJson);
			Payload->SetArrayField(TEXT("issues"), Issues);
		}
		else
		{
			TArray<FString> Packages;
			FString Error;
			if (!ExpandTargets(Targets, true, Packages, Error))
			{
				return ToolFailure(TEXT("Sync.InvalidTarget"), Error);
			}
			if (Packages.Num() > MaxCheckedTargets)
			{
				return ToolFailure(TEXT("Sync.TooManyTargets"), FString::Printf(
					TEXT("%d assets; check at most %d at once (name a smaller folder), or call status without targets"),
					Packages.Num(), MaxCheckedTargets));
			}
			// Only a pair that changed since its last sync needs a (dry-run) pass; the packages those load are
			// released after every batch.
			TArray<TSharedPtr<FJsonValue>> Assets;
			int32 SinceCollection = 0;
			for (const FString& Package : Packages)
			{
				if (const TSharedPtr<FJsonObject> Quick = QuickStatusOf(*SidecarSync, *SidecarSyncRunner, Package, bEnabled))
				{
					Assets.Add(MakeShared<FJsonValueObject>(Quick.ToSharedRef()));
					continue;
				}
				Assets.Add(MakeShared<FJsonValueObject>(StatusOf(*SidecarSync, Package)));
				if (++SinceCollection >= FAssetDocumentSidecarSyncRunner::BatchSize)
				{
					CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
					SinceCollection = 0;
				}
			}
			if (SinceCollection > 0)
			{
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}
			Payload->SetArrayField(TEXT("assets"), Assets);
		}
		FAssetDocumentResult Result = FAssetDocumentResult::Success(bEnabled
			? FString(TEXT("Sidecar mirroring status"))
			: FString(TEXT("Sidecar mirroring is off (Project Settings > Plugins > UE-AssetDoc > Mirror Sidecars)")));
		Payload->SetBoolField(TEXT("enabled"), bEnabled);
		Result.Payload = Payload;
		return Result;
	}

	const bool bAcceptSidecar = Action == TEXT("accept_sidecar");
	const bool bAcceptAsset = Action == TEXT("accept_asset");
	const bool bMaterialize = Action == TEXT("materialize");
	if (!bAcceptSidecar && !bAcceptAsset && !bMaterialize)
	{
		return ToolFailure(TEXT("Sync.InvalidAction"), FString::Printf(
			TEXT("Unknown action '%s'; use status, accept_sidecar, accept_asset or materialize"), *Action));
	}
	if (!bEnabled)
	{
		return ToolFailure(TEXT("Sync.MirroringOff"),
			TEXT("Sidecar mirroring is off; the user turns it on in Project Settings > Plugins > UE-AssetDoc > Mirror Sidecars"));
	}
	if (!IsSidecarSyncAvailable())
	{
		return ToolFailure(TEXT("GenericRuntime.Inactive"), TEXT("The AssetDocument Python runtime is not available"));
	}

	if (bMaterialize)
	{
		TArray<FString> Packages;
		FString Error;
		if (!ExpandTargets(Targets.IsEmpty() ? TArray<FString>{TEXT("/Game")} : Targets, true, Packages, Error))
		{
			return ToolFailure(TEXT("Sync.InvalidTarget"), Error);
		}
		const int32 Total = SidecarSyncRunner->StartMaterialize(Packages);
		FAssetDocumentResult Result = FAssetDocumentResult::Success(FString::Printf(
			TEXT("Materializing %d assets in the background; health_check sidecar_sync shows progress, status the result"), Total));
		Payload->SetNumberField(TEXT("total"), Total);
		Payload->SetObjectField(TEXT("runner"), SidecarSyncRunner->GetStatusJson());
		Result.Payload = Payload;
		return Result;
	}

	if (Targets.IsEmpty())
	{
		return ToolFailure(TEXT("Sync.InvalidTarget"), TEXT("accept_sidecar and accept_asset need targets (the assets to resolve)"));
	}
	TArray<FString> Packages;
	FString Error;
	if (!ExpandTargets(Targets, false, Packages, Error))
	{
		return ToolFailure(TEXT("Sync.InvalidTarget"), Error);
	}
	TArray<TSharedPtr<FJsonValue>> Outcomes;
	TArray<FAssetDocumentDiagnostic> Problems;
	for (const FString& Package : Packages)
	{
		FAssetDocumentSidecarSyncRequest Request;
		Request.PackageName = Package;
		Request.Trigger = EAssetDocumentSidecarSyncTrigger::Explicit;
		Request.Accept = bAcceptSidecar ? EAssetDocumentSidecarSyncAccept::Sidecar : EAssetDocumentSidecarSyncAccept::Asset;
		Request.AcceptRegions = Regions;
		const FAssetDocumentSidecarSyncOutcome Outcome = SidecarSync->Sync(Request);
		Outcomes.Add(MakeShared<FJsonValueObject>(Outcome.ToJson()));
		if (Outcome.IsProblem() || Outcome.State == TEXT("deferred") || Outcome.State == TEXT("excluded"))
		{
			FAssetDocumentDiagnostic Diagnostic = FAssetDocumentSidecarSync::ToApplyResult(Outcome).Diagnostics.IsEmpty()
				? FAssetDocumentDiagnostic()
				: FAssetDocumentSidecarSync::ToApplyResult(Outcome).Diagnostics[0];
			if (Diagnostic.Code.IsEmpty())
			{
				Diagnostic.Code = Outcome.Code.IsEmpty() ? FString(TEXT("Sync.Excluded")) : Outcome.Code;
				Diagnostic.Message = Outcome.Message.IsEmpty() ? FString::Printf(TEXT("%s is not mirrored (%s)"), *Package, *Outcome.Reason) : Outcome.Message;
			}
			Diagnostic.Path = Package;
			Problems.Add(Diagnostic);
		}
	}
	Payload->SetArrayField(TEXT("results"), Outcomes);
	FAssetDocumentResult Result = Problems.IsEmpty()
		? FAssetDocumentResult::Success(bAcceptSidecar ? TEXT("Resolved: the sidecar was applied") : TEXT("Resolved: the sidecar was rewritten from the asset"))
		: FAssetDocumentResult::FailureFromDiagnostics(Problems, TEXT("Not every asset was resolved"));
	Result.Payload = Payload;
	return Result;
}
