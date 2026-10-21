// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentSidecarApplyLedger.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

FString FAssetDocumentSidecarApplyLedger::HashFile(const FString& FilePath)
{
	TArray<uint8> Bytes;
	if (FilePath.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *FilePath, FILEREAD_Silent))
	{
		return FString();
	}
	FSHAHash Hash;
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
	return Hash.ToString();
}

FAssetDocumentSidecarTargetState FAssetDocumentSidecarApplyLedger::CaptureTargetState(const FString& PackageName)
{
	FAssetDocumentSidecarTargetState State;
	if (PackageName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackageName))
	{
		return State;
	}
	FString Filename;
	if (FPackageName::DoesPackageExist(PackageName, &Filename))
	{
		const FFileStatData Stat = IFileManager::Get().GetStatData(*Filename);
		State.bOnDisk = Stat.bIsValid;
		State.FileSize = Stat.FileSize;
		State.FileTimestamp = Stat.ModificationTime;
	}
	if (const UPackage* Package = FindPackage(nullptr, *PackageName))
	{
		State.bLoaded = true;
		State.bDirty = Package->IsDirty();
#if WITH_EDITORONLY_DATA
		State.SavedHash = Package->GetSavedHash();
#endif
	}
	return State;
}

const FAssetDocumentSidecarApplyLedger::FRecord* FAssetDocumentSidecarApplyLedger::FindMatching(
	const FString& SidecarPath, const FString& ContentHash, const FString& PackageName) const
{
	const FRecord* Record = Records.Find(SidecarPath);
	if (Record == nullptr || ContentHash.IsEmpty() || !Record->ContentHashes.Contains(ContentHash)
		|| Record->PackageName != PackageName)
	{
		return nullptr;
	}
	return Record->TargetState == CaptureTargetState(PackageName) ? Record : nullptr;
}

void FAssetDocumentSidecarApplyLedger::Record(const FString& SidecarPath, TArray<FString> ContentHashes,
	const FString& PackageName, const FAssetDocumentResult& Result, bool bByWatcher)
{
	ContentHashes.RemoveAll([](const FString& Hash) { return Hash.IsEmpty(); });
	if (ContentHashes.IsEmpty())
	{
		Records.Remove(SidecarPath);
		return;
	}
	FRecord& Entry = Records.FindOrAdd(SidecarPath);
	Entry.ContentHashes = MoveTemp(ContentHashes);
	Entry.PackageName = PackageName;
	Entry.TargetState = CaptureTargetState(PackageName);
	Entry.Result = Result;
	if (Result.Payload.IsValid())
	{
		Entry.Result.Payload = MakeShared<FJsonObject>();
		Entry.Result.Payload->Values = Result.Payload->Values;
	}
	Entry.bByWatcher = bByWatcher;
	Entry.AppliedAtUtc = FDateTime::UtcNow();
}

FAssetDocumentResult FAssetDocumentSidecarApplyLedger::MakeReplayedWatcherResult(const FRecord& Record)
{
	FAssetDocumentResult Result = Record.Result;
	const FString AppliedAt = Record.AppliedAtUtc.ToIso8601();
	Result.Message = FString::Printf(
		TEXT("%s (applied by the editor's sidecar file watcher at %s when the file was written; this call returns that result and did not apply it again)"),
		*Record.Result.Message, *AppliedAt);
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	if (Record.Result.Payload.IsValid())
	{
		Payload->Values = Record.Result.Payload->Values;
	}
	Payload->SetStringField(TEXT("applied_by"), TEXT("file_watcher"));
	Payload->SetStringField(TEXT("applied_at_utc"), AppliedAt);
	Result.Payload = Payload;
	return Result;
}

FAssetDocumentResult FAssetDocumentSidecarApplyLedger::MakeSkippedWatcherResult(
	const FRecord& Record, const FString& SidecarPath)
{
	FAssetDocumentResult Result = FAssetDocumentResult::Success(FString::Printf(
		TEXT("Skipped AssetDocument auto apply for '%s': this content was already applied at %s and the target is unchanged since"),
		*SidecarPath, *Record.AppliedAtUtc.ToIso8601()));
	Result.Target = Record.Result.Target;
	Result.AssetPath = Record.Result.AssetPath;
	Result.SidecarFilePath = SidecarPath;
	Result.Payload = MakeShared<FJsonObject>();
	Result.Payload->SetStringField(TEXT("sidecar_file_path"), SidecarPath);
	Result.Payload->SetBoolField(TEXT("triggered_by_watcher"), true);
	Result.Payload->SetBoolField(TEXT("skipped_already_applied"), true);
	return Result;
}
