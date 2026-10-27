// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentSidecarSync.h"

#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"
#include "AssetDocumentEditorSync.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentPendingNotices.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarSyncRunner.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace SidecarSyncLocal
{
TSet<FString> GActivePackages;
int32 GExtractCount = 0;
FString GStopAfterStep;
TFunction<void(const FString&)> GStepHook;

void RunStepHook(const TCHAR* Step)
{
	if (GStepHook)
	{
		GStepHook(Step);
	}
}

struct FEvidence
{
	TMap<FString, FString> Regions;
	FString AbsentHash;
	TArray<uint8> Bytes;
};

struct FFailure
{
	FString Code;
	FString Message;
};

bool ShouldStopAfter(const TCHAR* Step)
{
	if (!GStopAfterStep.IsEmpty() && GStopAfterStep == Step)
	{
		GStopAfterStep.Reset();
		return true;
	}
	return false;
}

FString BytesToText(TConstArrayView<uint8> Bytes)
{
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	return FString(Converted.Length(), Converted.Get());
}

FFailure FirstFailure(const TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics, const TCHAR* FallbackCode, const FString& FallbackMessage)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		if (Diagnostic.Severity == EAssetDocumentDiagnosticSeverity::Error || Diagnostics.Num() == 1)
		{
			return {Diagnostic.Code, Diagnostic.Path.IsEmpty() ? Diagnostic.Message : FString::Printf(TEXT("%s (at %s)"), *Diagnostic.Message, *Diagnostic.Path)};
		}
	}
	return {FallbackCode, FallbackMessage};
}

const FAssetDocumentRuntimeValueDto* FindField(const FAssetDocumentRuntimeValueDto& Value, const TCHAR* Name)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return nullptr;
	}
	const int32 Index = Value.ObjectKeys.IndexOfByKey(FString(Name));
	return Index == INDEX_NONE || !Value.Elements.IsValidIndex(Index) ? nullptr : &Value.Elements[Index];
}

bool Evaluate(FAssetDocumentService& Service, const TSharedRef<FJsonObject>& Options, FEvidence& Out, FFailure& OutFailure)
{
	const FAssetDocumentRuntimeResultDto Result = Service.DispatchSidecarSync(Options);
	if (Result.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded || !Result.CanonicalValue.IsSet())
	{
		OutFailure = FirstFailure(Result.Diagnostics, TEXT("Sync.EvidenceUnavailable"), TEXT("The sidecar sync evidence could not be computed."));
		return false;
	}
	const FAssetDocumentRuntimeValueDto* Regions = FindField(Result.CanonicalValue.GetValue(), TEXT("Regions"));
	const FAssetDocumentRuntimeValueDto* Absent = FindField(Result.CanonicalValue.GetValue(), TEXT("AbsentHash"));
	if (Regions == nullptr || Absent == nullptr || Regions->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		OutFailure = {TEXT("Sync.EvidenceUnavailable"), TEXT("The sidecar sync evidence has an unexpected shape.")};
		return false;
	}
	Out.Regions.Reset();
	for (int32 Index = 0; Index < Regions->ObjectKeys.Num() && Index < Regions->Elements.Num(); ++Index)
	{
		Out.Regions.Add(Regions->ObjectKeys[Index], Regions->Elements[Index].StringValue);
	}
	Out.AbsentHash = Absent->StringValue;
	Out.Bytes = Result.CanonicalBytes;
	return true;
}

TSharedRef<FJsonObject> MakeOptions(const TCHAR* Mode, const FString& Text)
{
	const TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
	Options->SetStringField(TEXT("Mode"), Mode);
	Options->SetStringField(TEXT("Text"), Text);
	return Options;
}

void SetRegions(const TSharedRef<FJsonObject>& Options, const TArray<FString>& Regions)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Region : Regions)
	{
		Values.Add(MakeShared<FJsonValueString>(Region));
	}
	Options->SetArrayField(TEXT("Regions"), Values);
}

TSharedPtr<FJsonObject> ParseObject(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object))
	{
		return nullptr;
	}
	return Object;
}

bool IsEmptyJson(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		return true;
	}
	if (Value->Type == EJson::Array)
	{
		return Value->AsArray().IsEmpty();
	}
	if (Value->Type == EJson::Object)
	{
		return Value->AsObject()->Values.IsEmpty();
	}
	return false;
}

// An extract that authors nothing: no Properties and every Body region null or empty.
bool IsEmptyExtract(const FJsonObject& Document)
{
	const TSharedPtr<FJsonValue> Properties = Document.TryGetField(TEXT("Properties"));
	if (!IsEmptyJson(Properties))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Body = nullptr;
	if (Document.TryGetObjectField(TEXT("Body"), Body) && Body && Body->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Region : (*Body)->Values)
		{
			if (Region.Key != TEXT("_Skipped") && !IsEmptyJson(Region.Value))
			{
				return false;
			}
		}
	}
	const TSharedPtr<FJsonObject>* Definitions = nullptr;
	return !(Document.TryGetObjectField(TEXT("Definitions"), Definitions) && Definitions && Definitions->IsValid()
		&& !(*Definitions)->Values.IsEmpty());
}

// The regions a diff of the sidecar against the asset reports as different (the whole document for a would-create).
TSet<FString> DifferingRegions(const FAssetDocumentResult& Diff, const TArray<FString>& Candidates, const TArray<FString>& AllRegions)
{
	TSet<FString> Differing;
	if (!Diff.IsSuccess() || !Diff.Payload.IsValid())
	{
		Differing.Append(Candidates);
		return Differing;
	}
	bool bWouldCreate = false;
	if (Diff.Payload->TryGetBoolField(TEXT("would_create"), bWouldCreate) && bWouldCreate)
	{
		Differing.Append(Candidates);
		return Differing;
	}
	for (const TCHAR* Field : {TEXT("changed"), TEXT("added"), TEXT("removed"), TEXT("failed")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Diff.Payload->TryGetArrayField(Field, Entries))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Entries)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Path;
			if (!Entry.IsValid() || !Entry->TryGetObject(Object) || !Object || !(*Object)->TryGetStringField(TEXT("path"), Path))
			{
				Differing.Append(Candidates);
				return Differing;
			}
			const bool bWholeDocument = Path.IsEmpty() || Path == TEXT("/");
			bool bInARegion = false;
			for (const FString& Region : AllRegions)
			{
				if (bWholeDocument || Path == Region || Path.StartsWith(Region + TEXT("/")))
				{
					bInARegion = true;
					if (Candidates.Contains(Region))
					{
						Differing.Add(Region);
					}
				}
			}
			if (!bInARegion)
			{
				// A change that names no region (a definition, or a path of another form) may be in any of them.
				Differing.Append(Candidates);
			}
		}
	}
	return Differing;
}

struct FScopedActivePackage
{
	explicit FScopedActivePackage(const FString& InPackage)
		: Package(InPackage.ToLower())
	{
		GActivePackages.Add(Package);
	}
	~FScopedActivePackage()
	{
		GActivePackages.Remove(Package);
	}
	FString Package;
};

enum class ERegionDecision : uint8
{
	NoChange,
	SidecarChanged,
	AssetChanged,
	Converged,
	NeedsComparison,
};
}

bool FAssetDocumentSidecarSyncOutcome::IsProblem() const
{
	return State == TEXT("conflict") || State == TEXT("unavailable") || State == TEXT("failed") || State == TEXT("asset_missing");
}

TSharedRef<FJsonObject> FAssetDocumentSidecarSyncOutcome::ToJson() const
{
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	auto Strings = [](const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	};
	Json->SetStringField(TEXT("asset"), PackageName);
	Json->SetStringField(TEXT("state"), State);
	if (!SidecarPath.IsEmpty())
	{
		Json->SetStringField(TEXT("sidecar"), SidecarPath);
	}
	if (!Code.IsEmpty())
	{
		Json->SetStringField(TEXT("code"), Code);
	}
	if (!Reason.IsEmpty())
	{
		Json->SetStringField(TEXT("reason"), Reason);
	}
	if (!Message.IsEmpty())
	{
		Json->SetStringField(TEXT("message"), Message);
	}
	if (!ConflictRegions.IsEmpty())
	{
		Json->SetArrayField(TEXT("conflict_regions"), Strings(ConflictRegions));
		Json->SetStringField(TEXT("conflict_file"), ConflictFile);
	}
	if (!SidecarChangedRegions.IsEmpty())
	{
		Json->SetArrayField(TEXT("sidecar_changed"), Strings(SidecarChangedRegions));
	}
	if (!AssetChangedRegions.IsEmpty())
	{
		Json->SetArrayField(TEXT("asset_changed"), Strings(AssetChangedRegions));
	}
	return Json;
}

FAssetDocumentSidecarSync::FAssetDocumentSidecarSync(FAssetDocumentService& InService)
	: Service(InService)
	, Index(FAssetDocumentSidecarSyncIndex::GetDefaultFilePath())
{
	Index.Load();
}

FAssetDocumentSidecarSync::~FAssetDocumentSidecarSync()
{
	if (Index.IsDirty())
	{
		Index.Save();
	}
}

bool FAssetDocumentSidecarSync::IsSyncing(const FString& PackageName)
{
	return SidecarSyncLocal::GActivePackages.Contains(UE::AssetDocument::SidecarSync::ToPackageName(PackageName).ToLower());
}

int32 FAssetDocumentSidecarSync::GetExtractCountForTest()
{
	return SidecarSyncLocal::GExtractCount;
}

void FAssetDocumentSidecarSync::StopAfterStepForTest(const TCHAR* Step)
{
	SidecarSyncLocal::GStopAfterStep = Step ? Step : TEXT("");
}

void FAssetDocumentSidecarSync::SetStepHookForTest(TFunction<void(const FString& Step)> Hook)
{
	SidecarSyncLocal::GStepHook = MoveTemp(Hook);
}

void FAssetDocumentSidecarSync::RecordIndex(const FAssetDocumentSidecarSyncOutcome& Outcome)
{
	using namespace UE::AssetDocument::SidecarSync;
	FAssetDocumentSidecarSyncIndexEntry Entry;
	if (const FAssetDocumentSidecarSyncIndexEntry* Existing = Index.Find(Outcome.PackageName))
	{
		Entry = *Existing;
	}
	Entry.State = Outcome.State;
	Entry.Reason = !Outcome.Reason.IsEmpty() ? Outcome.Reason : Outcome.Code;
	Entry.PackageHash = GetPackageSavedHash(Outcome.PackageName);
	const FFileStatData Stat = Outcome.SidecarPath.IsEmpty() ? FFileStatData() : IFileManager::Get().GetStatData(*Outcome.SidecarPath);
	if (Stat.bIsValid && !Stat.bIsDirectory)
	{
		Entry.FileSize = Stat.FileSize;
		Entry.FileTimestamp = Stat.ModificationTime;
		Entry.FileDigest = DigestSidecarFile(Outcome.SidecarPath);
	}
	else
	{
		Entry.FileSize = INDEX_NONE;
		Entry.FileTimestamp = FDateTime();
		Entry.FileDigest.Reset();
	}
	Index.Set(Outcome.PackageName, Entry);
}

FAssetDocumentSidecarSyncOutcome FAssetDocumentSidecarSync::Sync(const FAssetDocumentSidecarSyncRequest& Request)
{
	using namespace SidecarSyncLocal;
	using namespace UE::AssetDocument::SidecarSync;

	FAssetDocumentSidecarSyncOutcome Outcome;
	Outcome.PackageName = ToPackageName(Request.PackageName);
	const FString& PackageName = Outcome.PackageName;
	const FScopedActivePackage Active(PackageName);
	const bool bDryRun = Request.bDryRun;
	const bool bWatcher = Request.Trigger == EAssetDocumentSidecarSyncTrigger::SidecarChanged;

	auto Finish = [this, &Outcome, bDryRun]() -> FAssetDocumentSidecarSyncOutcome
	{
		if (!bDryRun)
		{
			RecordIndex(Outcome);
		}
		return Outcome;
	};
	auto Notice = [&Outcome, bDryRun](const TCHAR* Kind, const FString& Code, const FString& Message, bool bShow)
	{
		if (bDryRun)
		{
			return;
		}
		UE_LOG(LogAssetDocument, Warning, TEXT("AssetDocument sidecar sync of %s: [%s] %s"), *Outcome.PackageName, *Code, *Message);
		FAssetDocumentPendingNotices::Post(Kind, Outcome.SidecarPath, Code, Message,
			bShow ? FString::Printf(TEXT("AssetDocument: %s"), *Message) : FString());
	};
	auto ResolveNotices = [&Outcome, bDryRun]()
	{
		if (!bDryRun)
		{
			FAssetDocumentPendingNotices::Resolve(ConflictNotice, Outcome.SidecarPath);
			FAssetDocumentPendingNotices::Resolve(FailedNotice, Outcome.SidecarPath);
		}
	};
	auto Unavailable = [&](const FFailure& Failure, const TCHAR* Side)
	{
		Outcome.State = TEXT("unavailable");
		Outcome.Code = Failure.Code;
		Outcome.Message = FString::Printf(TEXT("The %s could not be read for sync, nothing was written: %s"), Side, *Failure.Message);
		Notice(FailedNotice, Outcome.Code, Outcome.Message, true);
		return Finish();
	};

	Outcome.SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(PackageName);
	FString ScopeReason;
	if (Outcome.SidecarPath.IsEmpty() || !IsInMirrorScope(PackageName, &ScopeReason))
	{
		Outcome.State = TEXT("excluded");
		Outcome.Reason = ScopeReason.IsEmpty() ? FString(TEXT("not_game")) : ScopeReason;
		return Outcome;
	}
	Outcome.SidecarPath = FPaths::ConvertRelativePathToFull(Outcome.SidecarPath);
	FPaths::NormalizeFilename(Outcome.SidecarPath);
	Outcome.ConflictFile = FAssetDocumentSidecar::ConflictFilePath(Outcome.SidecarPath);

	const UPackage* LoadedPackage = FindPackage(nullptr, *PackageName);
	const bool bPackage = LoadedPackage != nullptr || FPackageName::DoesPackageExist(PackageName);
	const bool bSidecar = IFileManager::Get().FileExists(*Outcome.SidecarPath);
	if (LoadedPackage != nullptr && LoadedPackage->IsDirty())
	{
		Outcome.State = TEXT("deferred");
		Outcome.Code = TEXT("Sync.TargetDirty");
		Outcome.Message = TEXT("The asset has unsaved changes; the mirror follows its saved state, so it syncs at the next save.");
		return Outcome;
	}

	// The sidecar side. ExpectedDigest is the file's content as this pass knows it (read, then as it last wrote it;
	// empty: no file): each write and the apply first check that the file still has it.
	FString SidecarText;
	FString ExpectedDigest;
	TSharedPtr<FJsonObject> SidecarJson;
	TOptional<FAssetDocumentSidecarSyncMeta> Meta;
	FEvidence SidecarEvidence;
	if (bSidecar)
	{
		FString Error;
		TArray<uint8> SidecarBytes;
		if (!FAssetDocumentSidecar::LoadBytes(Outcome.SidecarPath, SidecarBytes, Error))
		{
			return Unavailable({TEXT("Sync.SidecarUnreadable"), Error}, TEXT("sidecar"));
		}
		ExpectedDigest = SidecarBytes.Num() > GetMaxSidecarDigestBytes() ? DigestSidecarFile(Outcome.SidecarPath) : DigestBytes(SidecarBytes);
		FFileHelper::BufferToString(SidecarText, SidecarBytes.GetData(), SidecarBytes.Num());
		FFailure Failure;
		if (!Evaluate(Service, MakeOptions(TEXT("evidence"), SidecarText), SidecarEvidence, Failure))
		{
			return Unavailable(Failure, TEXT("sidecar"));
		}
		SidecarJson = ParseObject(SidecarText);
		if (!SidecarJson.IsValid())
		{
			return Unavailable({TEXT("Sync.InvalidSidecar"), TEXT("The file is not a JSON object.")}, TEXT("sidecar"));
		}
		Meta = FAssetDocumentSidecarSyncMeta::Read(*SidecarJson);
	}
	{
		const FString Key = FAssetDocumentSidecarSyncIndex::MakeKey(PackageName);
		if (const FAssetDocumentSidecarSyncMeta* Pending = PendingBaselines.Find(Key))
		{
			if (bSidecar && Pending->Package == GetPackageSavedHash(PackageName))
			{
				Meta = *Pending;
			}
			if (!bDryRun)
			{
				PendingBaselines.Remove(Key);
			}
		}
	}

	// The sidecar changed after this pass read it (or last wrote it): nothing more is written; the new content gets
	// its own pass.
	bool bChangedDuringPass = false;
	auto IsAsKnown = [&]() -> bool
	{
		if (DigestSidecarFile(Outcome.SidecarPath) == ExpectedDigest)
		{
			return true;
		}
		bChangedDuringPass = true;
		Outcome.State = TEXT("changed");
		Outcome.Code = TEXT("Sync.ChangedDuringSync");
		Outcome.Message = TEXT("The sidecar changed while it was being synced; nothing more was written, the new content is kept and syncs next.");
		return false;
	};
	auto Requeue = [&]() -> FAssetDocumentSidecarSyncOutcome
	{
		UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument sidecar sync of %s: the sidecar changed during the pass; synced again next"), *PackageName);
		Service.GetSidecarSyncRunner().Enqueue(PackageName, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
		return Outcome;
	};

	auto RunApply = [&]() -> bool
	{
		RunStepHook(TEXT("before_apply"));
		if (!IsAsKnown())
		{
			return false;
		}
		const uint64 WritesBefore = FAssetDocumentEditorSync::GetSidecarWriteSerial();
		FAssetDocumentApplyFileRequest Apply;
		Apply.FilePath = Outcome.SidecarPath;
		Apply.bSaveAsset = true;
		Apply.bTriggeredByWatcher = bWatcher;
		Apply.bBypassSidecarSync = true;
		Outcome.Apply = Service.ApplyFile(Apply);
		if (!Outcome.Apply->IsSuccess())
		{
			Outcome.State = TEXT("failed");
			Outcome.Code = Outcome.Apply->Diagnostics.IsEmpty() ? FString(TEXT("Sync.ApplyFailed")) : Outcome.Apply->Diagnostics[0].Code;
			Outcome.Message = FString::Printf(TEXT("Applying the sidecar failed: %s"), *Outcome.Apply->Message);
			if (!bWatcher)
			{
				Notice(FailedNotice, Outcome.Code, Outcome.Message, false);
			}
			return false;
		}
		// The apply rewrites the file (canonical form): that is now the content this pass knows.
		if (const TOptional<FString> Written = FAssetDocumentEditorSync::FindSidecarWriteSince(Outcome.SidecarPath, WritesBefore))
		{
			ExpectedDigest = Written.GetValue();
		}
		RunStepHook(TEXT("after_apply"));
		return true;
	};

	FString ExtractText;
	auto ExtractAsset = [&](FFailure& OutFailure) -> bool
	{
		++GExtractCount;
		const FAssetDocumentRuntimeResultDto Extracted = Service.ExtractForMirror(PackageName);
		if (Extracted.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded || Extracted.CanonicalBytes.IsEmpty())
		{
			OutFailure = FirstFailure(Extracted.Diagnostics, TEXT("Sync.ExtractFailed"), TEXT("The asset could not be extracted."));
			return false;
		}
		ExtractText = BytesToText(Extracted.CanonicalBytes);
		return true;
	};

	auto WriteSidecar = [&](TConstArrayView<uint8> Bytes) -> bool
	{
		RunStepHook(TEXT("before_write"));
		if (!IsAsKnown())
		{
			return false;
		}
		const FString Digest = DigestBytes(Bytes);
		if (Digest == ExpectedDigest)
		{
			return true;
		}
		// The write replaces the file only while it still has the content checked above.
		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(Outcome.SidecarPath,
			FAssetDocumentEditorSync::FScopedSidecarWrite::ERecord::OwnWrites, ExpectedDigest);
		FString Error;
		if (!FAssetDocumentSidecar::WriteBytes(Outcome.SidecarPath, Bytes, Error))
		{
			if (!IsAsKnown())
			{
				return false;
			}
			Outcome.State = TEXT("unavailable");
			Outcome.Code = TEXT("Sync.WriteFailed");
			Outcome.Message = FString::Printf(TEXT("The sidecar could not be written: %s"), *Error);
			Notice(FailedNotice, Outcome.Code, Outcome.Message, true);
			return false;
		}
		ExpectedDigest = Digest;
		Outcome.bWroteSidecar = true;
		return true;
	};

	// Writes the asset's mirror (stamping the sync state) and clears the conflict file and notices.
	auto FinishMirror = [&](const TCHAR* State) -> FAssetDocumentSidecarSyncOutcome
	{
		FEvidence Mirror;
		FFailure Failure;
		const TSharedRef<FJsonObject> Options = MakeOptions(TEXT("mirror"), ExtractText);
		Options->SetStringField(TEXT("Package"), GetPackageSavedHash(PackageName));
		if (!Evaluate(Service, Options, Mirror, Failure))
		{
			return Unavailable(Failure, TEXT("asset"));
		}
		if (!WriteSidecar(Mirror.Bytes))
		{
			if (!bChangedDuringPass)
			{
				return Finish();
			}
			if (Outcome.Apply.IsSet())
			{
				// The asset now has the applied content: the next pass compares the new content with that.
				FAssetDocumentSidecarSyncMeta Applied;
				Applied.Package = GetPackageSavedHash(PackageName);
				for (const TPair<FString, FString>& Region : Mirror.Regions)
				{
					Applied.Regions.Add(Region.Key, {Region.Value, Region.Value});
				}
				PendingBaselines.Add(FAssetDocumentSidecarSyncIndex::MakeKey(PackageName), MoveTemp(Applied));
			}
			return Requeue();
		}
		if (Outcome.Apply.IsSet())
		{
			// The mirror is the file the apply left: an explicit apply of it returns the apply's result.
			Service.NoteSidecarRewrite(Outcome.SidecarPath, ExpectedDigest);
		}
		if (!Outcome.ConflictFile.IsEmpty() && IFileManager::Get().FileExists(*Outcome.ConflictFile))
		{
			IFileManager::Get().Delete(*Outcome.ConflictFile, false, true, true);
		}
		ResolveNotices();
		Outcome.State = State;
		return Finish();
	};

	if (!bPackage)
	{
		if (!bSidecar)
		{
			Outcome.State = TEXT("absent");
			return Outcome;
		}
		if (Meta.IsSet())
		{
			Outcome.State = TEXT("asset_missing");
			Outcome.Code = TEXT("Sync.AssetMissing");
			Outcome.Message = TEXT("The sidecar was synced with an asset that no longer exists; it is not recreated. Delete the sidecar, or apply it with sync_asset_document accept_sidecar to recreate the asset.");
			if (Request.Accept != EAssetDocumentSidecarSyncAccept::Sidecar)
			{
				Notice(FailedNotice, Outcome.Code, Outcome.Message, false);
				return Finish();
			}
		}
		if (bDryRun)
		{
			Outcome.State = TEXT("would_apply");
			return Outcome;
		}
		FFailure Failure;
		if (!RunApply())
		{
			return bChangedDuringPass ? Requeue() : Finish();
		}
		if (!ExtractAsset(Failure))
		{
			return Unavailable(Failure, TEXT("asset"));
		}
		return FinishMirror(TEXT("applied"));
	}

	if (!bSidecar)
	{
		if (Request.Trigger == EAssetDocumentSidecarSyncTrigger::SidecarChanged)
		{
			Outcome.State = TEXT("absent");
			return Outcome;
		}
		FFailure Failure;
		if (!ExtractAsset(Failure))
		{
			Outcome.State = TEXT("skipped");
			Outcome.Reason = TEXT("extract_failed");
			Outcome.Code = Failure.Code;
			Outcome.Message = Failure.Message;
			return Finish();
		}
		const TSharedPtr<FJsonObject> Extracted = ParseObject(ExtractText);
		if (Extracted.IsValid() && IsEmptyExtract(*Extracted))
		{
			Outcome.State = TEXT("skipped");
			Outcome.Reason = TEXT("empty");
			return Finish();
		}
		if (FTCHARToUTF8(*ExtractText).Length() > GetMaxMirrorBytes())
		{
			Outcome.State = TEXT("skipped");
			Outcome.Reason = TEXT("too_large");
			return Finish();
		}
		if (bDryRun)
		{
			Outcome.State = TEXT("would_write");
			return Outcome;
		}
		return FinishMirror(TEXT("wrote"));
	}

	// The asset side: without an extract when the package is the one the last sync saw.
	const FString PackageHash = GetPackageSavedHash(PackageName);
	const FString Prefix = FAssetDocumentSidecarSyncMeta::HashPrefixOf(SidecarEvidence.AbsentHash);
	// A last state is usable when every recorded hash is of the current codec. It lists every region either side had,
	// so a region it does not list was absent on both sides.
	const bool bTrustedMeta = Meta.IsSet() && Algo::AllOf(Meta->Regions,
		[&Meta, &Prefix](const TPair<FString, FAssetDocumentSidecarRegionState>& Region)
		{
			return Meta->FindTrusted(Region.Key, Prefix) != nullptr;
		});
	const bool bAssetByHash = bTrustedMeta && !Meta->Package.IsEmpty() && Meta->Package == PackageHash;
	FEvidence AssetEvidence;
	if (bAssetByHash)
	{
		for (const TPair<FString, FAssetDocumentSidecarRegionState>& Region : Meta->Regions)
		{
			AssetEvidence.Regions.Add(Region.Key, Region.Value.Asset);
		}
		AssetEvidence.AbsentHash = SidecarEvidence.AbsentHash;
	}
	else
	{
		FFailure Failure;
		if (!ExtractAsset(Failure) || !Evaluate(Service, MakeOptions(TEXT("evidence"), ExtractText), AssetEvidence, Failure))
		{
			return Unavailable(Failure, TEXT("asset"));
		}
	}

	TArray<FString> Regions;
	SidecarEvidence.Regions.GetKeys(Regions);
	for (const TPair<FString, FString>& Region : AssetEvidence.Regions)
	{
		Regions.AddUnique(Region.Key);
	}
	Regions.Sort();

	// Without named regions a resolution applies to the regions in conflict, so a one-way change elsewhere is kept.
	const bool bAcceptAll = Request.AcceptRegions.IsEmpty();
	auto Classify = [&SidecarEvidence](const FString& SidecarHash, const FString& AssetHash,
		const TOptional<FAssetDocumentSidecarRegionState>& Baseline)
	{
		if (!Baseline.IsSet())
		{
			return SidecarHash == AssetHash ? ERegionDecision::Converged
				: SidecarHash == SidecarEvidence.AbsentHash ? ERegionDecision::AssetChanged
				: ERegionDecision::NeedsComparison;
		}
		const bool bSidecarChanged = SidecarHash != Baseline->Sidecar;
		const bool bAssetChanged = AssetHash != Baseline->Asset;
		return !bSidecarChanged && !bAssetChanged ? ERegionDecision::NoChange
			: bSidecarChanged && !bAssetChanged ? ERegionDecision::SidecarChanged
			: !bSidecarChanged ? ERegionDecision::AssetChanged
			: SidecarHash == AssetHash ? ERegionDecision::Converged
			: ERegionDecision::NeedsComparison;
	};
	TMap<FString, ERegionDecision> Decisions;
	TArray<FString> NeedsComparison;
	for (const FString& Region : Regions)
	{
		const FString* SidecarHashPtr = SidecarEvidence.Regions.Find(Region);
		const FString* AssetHashPtr = AssetEvidence.Regions.Find(Region);
		const FString SidecarHash = SidecarHashPtr ? *SidecarHashPtr : SidecarEvidence.AbsentHash;
		const FString AssetHash = AssetHashPtr ? *AssetHashPtr : SidecarEvidence.AbsentHash;
		TOptional<FAssetDocumentSidecarRegionState> Baseline;
		if (bTrustedMeta)
		{
			const FAssetDocumentSidecarRegionState* Recorded = Meta->FindTrusted(Region, Prefix);
			Baseline = Recorded ? *Recorded : FAssetDocumentSidecarRegionState{SidecarEvidence.AbsentHash, SidecarEvidence.AbsentHash};
		}
		ERegionDecision Decision = Classify(SidecarHash, AssetHash, Baseline);
		const bool bAccepted = Request.Accept != EAssetDocumentSidecarSyncAccept::None
			&& (bAcceptAll ? Decision == ERegionDecision::NeedsComparison : Request.AcceptRegions.Contains(Region));
		if (bAccepted)
		{
			// An accepted region whose sides differ takes the accepted side, whichever side changed since the last sync.
			Decision = SidecarHash == AssetHash ? ERegionDecision::Converged
				: Request.Accept == EAssetDocumentSidecarSyncAccept::Sidecar ? ERegionDecision::SidecarChanged
				: ERegionDecision::AssetChanged;
		}
		if (Decision == ERegionDecision::NeedsComparison)
		{
			NeedsComparison.Add(Region);
		}
		Decisions.Add(Region, Decision);
	}

	if (!NeedsComparison.IsEmpty())
	{
		// Both sides moved (or there is no last state): equal content is converged, anything else a conflict.
		TSet<FString> Differing;
		if (NeedsComparison.Contains(TEXT("/Class")))
		{
			Differing.Add(TEXT("/Class"));
		}
		const TArray<FString> Compared = NeedsComparison.FilterByPredicate([](const FString& Region) { return Region != TEXT("/Class"); });
		if (!Compared.IsEmpty())
		{
			FAssetDocumentDiffRequest Diff;
			Diff.Document = SidecarJson;
			Differing.Append(DifferingRegions(Service.Diff(Diff), Compared, Regions));
		}
		for (const FString& Region : NeedsComparison)
		{
			if (Differing.Contains(Region))
			{
				Outcome.ConflictRegions.Add(Region);
			}
			else
			{
				Decisions[Region] = ERegionDecision::Converged;
			}
		}
	}

	for (const TPair<FString, ERegionDecision>& Decision : Decisions)
	{
		if (Decision.Value == ERegionDecision::SidecarChanged)
		{
			Outcome.SidecarChangedRegions.Add(Decision.Key);
		}
		else if (Decision.Value == ERegionDecision::AssetChanged)
		{
			Outcome.AssetChangedRegions.Add(Decision.Key);
		}
	}

	if (!Outcome.ConflictRegions.IsEmpty())
	{
		Outcome.State = TEXT("conflict");
		Outcome.Code = bTrustedMeta ? TEXT("Sync.Conflict") : TEXT("Sync.InitConflict");
		Outcome.Message = FString::Printf(
			TEXT("%s: %s changed in both the sidecar and the asset%s. Nothing was written; the asset's version is in %s. ")
			TEXT("Edit the sidecar to the wanted result, then sync_asset_document accept_sidecar (or accept_asset to take the asset)."),
			*PackageName, *FString::Join(Outcome.ConflictRegions, TEXT(", ")),
			bTrustedMeta ? TEXT(" since the last sync") : TEXT(" (the sidecar has no usable sync state)"),
			*FPaths::GetCleanFilename(Outcome.ConflictFile));
		if (bDryRun)
		{
			return Outcome;
		}
		FFailure Failure;
		if (ExtractText.IsEmpty() && !ExtractAsset(Failure))
		{
			return Unavailable(Failure, TEXT("asset"));
		}
		FEvidence Conflict;
		const TSharedRef<FJsonObject> Options = MakeOptions(TEXT("conflict"), ExtractText);
		SetRegions(Options, Outcome.ConflictRegions);
		FString Error;
		if (!Evaluate(Service, Options, Conflict, Failure)
			|| !FAssetDocumentSidecar::WriteConflictFile(Outcome.SidecarPath, Conflict.Bytes, Error))
		{
			Outcome.Message += FString::Printf(TEXT(" (The conflict file could not be written: %s)"),
				Error.IsEmpty() ? *Failure.Message : *Error);
		}
		Notice(ConflictNotice, Outcome.Code, Outcome.Message, true);
		return Finish();
	}

	const bool bConverged = Algo::AnyOf(Decisions, [](const TPair<FString, ERegionDecision>& Decision)
	{
		return Decision.Value == ERegionDecision::Converged;
	});
	const bool bNeedsStamp = !bTrustedMeta || Meta->Package != PackageHash
		|| Algo::AnyOf(Regions, [&](const FString& Region) { return Meta->FindTrusted(Region, Prefix) == nullptr; });
	if (Outcome.SidecarChangedRegions.IsEmpty() && Outcome.AssetChangedRegions.IsEmpty() && !bConverged && !bNeedsStamp)
	{
		if (!bDryRun && !Outcome.ConflictFile.IsEmpty() && IFileManager::Get().FileExists(*Outcome.ConflictFile))
		{
			IFileManager::Get().Delete(*Outcome.ConflictFile, false, true, true);
		}
		ResolveNotices();
		Outcome.State = TEXT("in_sync");
		return Finish();
	}
	if (bDryRun)
	{
		Outcome.State = Outcome.SidecarChangedRegions.IsEmpty() ? TEXT("would_write") : TEXT("would_apply");
		return Outcome;
	}

	FFailure Failure;
	if (!Outcome.SidecarChangedRegions.IsEmpty())
	{
		if (!Outcome.AssetChangedRegions.IsEmpty())
		{
			// The apply is of the whole file: the regions the asset changed go into it first, so the apply keeps them.
			FEvidence Merged;
			const TSharedRef<FJsonObject> Options = MakeOptions(TEXT("merge"), SidecarText);
			Options->SetStringField(TEXT("From"), ExtractText);
			SetRegions(Options, Outcome.AssetChangedRegions);
			if (!Evaluate(Service, Options, Merged, Failure))
			{
				return Unavailable(Failure, TEXT("sidecar"));
			}
			if (!WriteSidecar(Merged.Bytes))
			{
				return bChangedDuringPass ? Requeue() : Finish();
			}
			if (ShouldStopAfter(TEXT("merge")))
			{
				Outcome.State = TEXT("interrupted");
				return Outcome;
			}
		}
		if (!RunApply())
		{
			return bChangedDuringPass ? Requeue() : Finish();
		}
		if (ShouldStopAfter(TEXT("apply")))
		{
			Outcome.State = TEXT("interrupted");
			return Outcome;
		}
		if (!ExtractAsset(Failure))
		{
			return Unavailable(Failure, TEXT("asset"));
		}
		return FinishMirror(TEXT("applied"));
	}
	if (ExtractText.IsEmpty() && !ExtractAsset(Failure))
	{
		return Unavailable(Failure, TEXT("asset"));
	}
	return FinishMirror(Outcome.AssetChangedRegions.IsEmpty() ? TEXT("in_sync") : TEXT("wrote"));
}

FAssetDocumentResult FAssetDocumentSidecarSync::ToApplyResult(const FAssetDocumentSidecarSyncOutcome& Outcome)
{
	FAssetDocumentResult Result;
	if (Outcome.Apply.IsSet())
	{
		Result = Outcome.Apply.GetValue();
	}
	else if (Outcome.IsProblem() || Outcome.State == TEXT("deferred"))
	{
		FAssetDocumentDiagnostic Diagnostic;
		Diagnostic.Path = TEXT("/");
		Diagnostic.Code = Outcome.Code.IsEmpty() ? FString(TEXT("Sync.Failed")) : Outcome.Code;
		Diagnostic.Message = Outcome.Message;
		Result = FAssetDocumentResult::FailureFromDiagnostics({Diagnostic}, FString::Printf(TEXT("%s: %s"), *Diagnostic.Code, *Outcome.Message));
	}
	else if (Outcome.State == TEXT("changed"))
	{
		Result = FAssetDocumentResult::Success(Outcome.Message);
	}
	else
	{
		Result = FAssetDocumentResult::Success(
			Outcome.State == TEXT("wrote")
				? FString(TEXT("The asset changed since the last sync and the sidecar did not: the sidecar was rewritten from the asset"))
				: FString(TEXT("The sidecar and the asset are in sync; nothing to apply")));
	}
	Result.SidecarFilePath = Outcome.SidecarPath;
	Result.Target = Outcome.PackageName;
	Result.bWroteSidecar = Result.bWroteSidecar || Outcome.bWroteSidecar;
	if (!Result.Payload.IsValid())
	{
		Result.Payload = MakeShared<FJsonObject>();
	}
	Result.Payload->SetObjectField(TEXT("sidecar_sync"), Outcome.ToJson());
	return Result;
}
