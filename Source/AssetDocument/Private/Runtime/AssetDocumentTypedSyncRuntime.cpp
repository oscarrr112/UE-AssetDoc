// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentTypedSyncRuntime.h"

#include "AssetDocumentAtomicFile.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentSidecarByteIo.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
constexpr uint32 TypedSyncWireVersion = 1;
const FString TypedSyncMagic = TEXT("ASSETDOC_TYPED_SYNC");

FAssetDocumentContractValidationResult MakeSyncResult(
	bool bValid,
	const TCHAR* Code = TEXT(""),
	const TCHAR* Message = TEXT(""))
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!Code[0])
	{
		return Result;
	}
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/TypedSync");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

void SerializeRuntimeValue(FArchive& Archive, FAssetDocumentRuntimeValueDto& Value)
{
	uint8 Kind = static_cast<uint8>(Value.Kind);
	Archive << Kind << Value.BooleanValue << Value.IntegerValue << Value.NumberValue;
	Archive << Value.StringValue << Value.BytesValue << Value.ObjectKeys;
	Value.Kind = static_cast<EAssetDocumentRuntimeValueKind>(Kind);
	int32 ElementCount = Value.Elements.Num();
	Archive << ElementCount;
	if (ElementCount < 0 || ElementCount > 4096)
	{
		Archive.SetError();
		return;
	}
	if (Archive.IsLoading())
	{
		Value.Elements.SetNum(ElementCount);
	}
	for (FAssetDocumentRuntimeValueDto& Element : Value.Elements)
	{
		SerializeRuntimeValue(Archive, Element);
	}
}

void SerializeDiagnosticContext(FArchive& Archive, FAssetDocumentRuntimeDiagnostic& Diagnostic)
{
	int32 ContextCount = Diagnostic.Context.Num();
	Archive << ContextCount;
	if (ContextCount < 0 || ContextCount > 4096)
	{
		Archive.SetError();
		return;
	}
	if (Archive.IsLoading())
	{
		Diagnostic.Context.SetNum(ContextCount);
	}
	for (FAssetDocumentRuntimeDiagnosticContext& Context : Diagnostic.Context)
	{
		Archive << Context.Key;
		SerializeRuntimeValue(Archive, Context.Value);
	}
}

void SerializeDiagnostic(FArchive& Archive, TOptional<FAssetDocumentRuntimeDiagnostic>& Diagnostic)
{
	bool bHasDiagnostic = Diagnostic.IsSet();
	Archive << bHasDiagnostic;
	if (Archive.IsLoading() && bHasDiagnostic)
	{
		Diagnostic.Emplace();
	}
	if (bHasDiagnostic)
	{
		Archive << Diagnostic->Path << Diagnostic->Code << Diagnostic->Message;
		uint8 Severity = static_cast<uint8>(Diagnostic->Severity);
		Archive << Severity;
		Diagnostic->Severity = static_cast<EAssetDocumentDiagnosticSeverity>(Severity);
		SerializeDiagnosticContext(Archive, Diagnostic.GetValue());
	}
}

void SerializeEvidence(FArchive& Archive, FAssetDocumentRegionEvidence& Evidence)
{
	uint8 Kind = static_cast<uint8>(Evidence.Kind);
	Archive << Kind << Evidence.SemanticHash << Evidence.CodecVersion;
	Evidence.Kind = static_cast<EAssetDocumentEvidenceKind>(Kind);
	SerializeDiagnostic(Archive, Evidence.Diagnostic);
}

bool SerializeTypedRecord(FArchive& Archive, FAssetDocumentTypedSyncStateRecord& Record)
{
	FString Magic = TypedSyncMagic;
	uint32 Version = TypedSyncWireVersion;
	Archive << Magic << Version;
	SerializeEvidence(Archive, Record.LastDurable.Asset);
	SerializeEvidence(Archive, Record.LastDurable.Sidecar);
	Archive << Record.RequestId << Record.TransactionId << Record.JournalId;
	return !Archive.IsError() && Magic == TypedSyncMagic && Version == TypedSyncWireVersion;
}

TArray<uint8> EncodeRecord(const FAssetDocumentTypedSyncStateRecord& Record)
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes, true);
	FAssetDocumentTypedSyncStateRecord MutableRecord = Record;
	SerializeTypedRecord(Writer, MutableRecord);
	Writer.Flush();
	return Bytes;
}

bool DecodeRecord(TArrayView64<const uint8> Bytes, FAssetDocumentTypedSyncStateRecord& OutRecord)
{
	TArray<uint8> OwnedBytes;
	OwnedBytes.Append(Bytes.GetData(), Bytes.Num());
	FMemoryReader Reader(OwnedBytes, true);
	return SerializeTypedRecord(Reader, OutRecord) && Reader.Tell() == Reader.TotalSize();
}

FAssetDocumentContractValidationResult ValidateKey(const FAssetDocumentSyncStateKey& Key)
{
	if (Key.CanonicalAssetPath.IsEmpty() || Key.RegionId.IsEmpty())
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Typed sync state key is incomplete."));
	}
	return MakeSyncResult(true);
}

FAssetDocumentContractValidationResult ValidateComparablePair(const FAssetDocumentSyncEvidencePair& Pair)
{
	FAssetDocumentContractValidationResult Result = ValidateAssetDocumentRegionEvidence(Pair.Asset);
	if (!Result.bValid)
	{
		return Result;
	}
	Result = ValidateAssetDocumentRegionEvidence(Pair.Sidecar);
	if (!Result.bValid)
	{
		return Result;
	}
	if (Pair.Asset.Kind == EAssetDocumentEvidenceKind::EvidenceUnavailable)
	{
		Result.bValid = false;
		Result.Diagnostics = {Pair.Asset.Diagnostic.GetValue()};
		return Result;
	}
	if (Pair.Sidecar.Kind == EAssetDocumentEvidenceKind::EvidenceUnavailable)
	{
		Result.bValid = false;
		Result.Diagnostics = {Pair.Sidecar.Diagnostic.GetValue()};
	}
	return Result;
}

FAssetDocumentContractValidationResult ValidateRecord(const FAssetDocumentTypedSyncStateRecord& Record)
{
	if (!Record.RequestId.IsValid() || !Record.TransactionId.IsValid() || Record.JournalId.IsEmpty())
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Typed sync state identity is incomplete."));
	}
	return ValidateComparablePair(Record.LastDurable);
}

bool HasReceiptIdentity(const FAssetDocumentDurableCompletionReceipt& Receipt)
{
	return Receipt.RequestId.IsValid() && Receipt.TransactionId.IsValid();
}

bool HasReceiptArtifacts(const FAssetDocumentDurableCompletionReceipt& Receipt)
{
	return !Receipt.JournalId.IsEmpty() && !Receipt.PackageContentDigest.IsEmpty()
		&& !Receipt.SidecarContentDigest.IsEmpty();
}

FAssetDocumentContractValidationResult ValidateCompletionReceipt(
	const FAssetDocumentDurableCompletionReceipt& Receipt)
{
	if (!HasReceiptIdentity(Receipt))
	{
		return MakeSyncResult(false, TEXT("Sync.MissingDurableCompletion"), TEXT("Persistence identity is incomplete before sync advancement."));
	}
	if (!HasReceiptArtifacts(Receipt))
	{
		return MakeSyncResult(false, TEXT("Sync.MissingDurableCompletion"), TEXT("Persistence artifacts are incomplete before sync advancement."));
	}
	return MakeSyncResult(true);
}

FString MakeStateFilename(const FAssetDocumentSyncStateKey& Key)
{
	const FString StableKey = Key.CanonicalAssetPath + TEXT("\n") + Key.RegionId;
	FTCHARToUTF8 Utf8(*StableKey);
	const FString Digest = ComputeAssetDocumentContentDigest(
		MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	return Digest.RightChop(7) + TEXT(".bin");
}

FAssetDocumentContractValidationResult RestoreAbsentState(const FString& Path)
{
	if (IFileManager::Get().FileExists(*Path)
		&& !IFileManager::Get().Delete(*Path, false, true, true))
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Absent typed sync baseline could not be restored."));
	}
	FString Error;
	if (!FAssetDocumentAtomicFile::FlushParentDirectory(Path, Error))
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Absent typed sync baseline could not be flushed."));
	}
	return MakeSyncResult(true);
}

FAssetDocumentContractValidationResult RestoreExistingState(
	const FString& Path,
	TArrayView64<const uint8> EncodedPreimage)
{
	FAssetDocumentTypedSyncStateRecord Record;
	if (!DecodeRecord(EncodedPreimage, Record))
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Typed sync preimage is corrupt."));
	}
	if (!ValidateRecord(Record).bValid)
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Typed sync preimage is corrupt."));
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	FString Error;
	if (!FAssetDocumentAtomicFile::WriteBytesAtomically(Path, EncodedPreimage, Error))
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Typed sync preimage could not be restored."));
	}
	return MakeSyncResult(true);
}

TArray<uint8> LoadExistingStateBytes(const FString& Path)
{
	TArray<uint8> Existing;
	if (IFileManager::Get().FileExists(*Path))
	{
		FFileHelper::LoadFileToArray(Existing, *Path);
	}
	return Existing;
}
}

FAssetDocumentJournaledTypedSyncStateStore::FAssetDocumentJournaledTypedSyncStateStore(FString InStateRoot)
	: StateRoot(FPaths::ConvertRelativePathToFull(MoveTemp(InStateRoot)))
{
	FPaths::NormalizeFilename(StateRoot);
}

FAssetDocumentContractValidationResult FAssetDocumentJournaledTypedSyncStateStore::Load(
	const FAssetDocumentSyncStateKey& Key,
	TOptional<FAssetDocumentTypedSyncStateRecord>& OutRecord,
	TArray<uint8>& OutEncodedBytes,
	bool& bOutExisted)
{
	const FAssetDocumentContractValidationResult KeyValidation = ValidateKey(Key);
	if (!KeyValidation.bValid)
	{
		return KeyValidation;
	}
	const FString Path = GetStatePath(Key);
	bOutExisted = IFileManager::Get().FileExists(*Path);
	OutRecord.Reset();
	OutEncodedBytes.Reset();
	if (!bOutExisted)
	{
		return MakeSyncResult(true);
	}
	FAssetDocumentTypedSyncStateRecord Record;
	if (!FFileHelper::LoadFileToArray(OutEncodedBytes, *Path) || !DecodeRecord(OutEncodedBytes, Record))
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Typed sync state record is corrupt."));
	}
	const FAssetDocumentContractValidationResult Validation = ValidateRecord(Record);
	OutRecord = MoveTemp(Record);
	return Validation;
}

FAssetDocumentContractValidationResult FAssetDocumentJournaledTypedSyncStateStore::Store(
	const FAssetDocumentSyncStateKey& Key,
	const FAssetDocumentTypedSyncStateRecord& Record)
{
	const FAssetDocumentContractValidationResult KeyValidation = ValidateKey(Key);
	if (!KeyValidation.bValid)
	{
		return KeyValidation;
	}
	const FAssetDocumentContractValidationResult RecordValidation = ValidateRecord(Record);
	if (!RecordValidation.bValid)
	{
		return RecordValidation;
	}
	const TArray<uint8> Bytes = EncodeRecord(Record);
	const TArray<uint8> Existing = LoadExistingStateBytes(GetStatePath(Key));
	if (Existing == Bytes)
	{
		return MakeSyncResult(true, TEXT("Sync.StateAlreadyCurrent"), TEXT("Typed sync state already matches installed evidence."));
	}
	IFileManager::Get().MakeDirectory(*StateRoot, true);
	FString Error;
	if (!FAssetDocumentAtomicFile::WriteBytesAtomically(GetStatePath(Key), Bytes, Error))
	{
		return MakeSyncResult(false, TEXT("Sync.StateAdvanceFailed"), TEXT("Typed sync state could not be written durably."));
	}
	return MakeSyncResult(true, TEXT("Sync.StateAdvanced"), TEXT("Typed sync state advanced after persistence artifacts."));
}

FAssetDocumentContractValidationResult FAssetDocumentJournaledTypedSyncStateStore::Restore(
	const FAssetDocumentSyncStateKey& Key,
	TArrayView64<const uint8> EncodedPreimage,
	bool bExisted)
{
	const FAssetDocumentContractValidationResult KeyValidation = ValidateKey(Key);
	if (!KeyValidation.bValid)
	{
		return KeyValidation;
	}
	const FString Path = GetStatePath(Key);
	if (bExisted)
	{
		return RestoreExistingState(Path, EncodedPreimage);
	}
	return RestoreAbsentState(Path);
}

FString FAssetDocumentJournaledTypedSyncStateStore::GetStatePath(
	const FAssetDocumentSyncStateKey& Key) const
{
	return FPaths::Combine(StateRoot, MakeStateFilename(Key));
}

FAssetDocumentSyncCoordinatorRuntime::FAssetDocumentSyncCoordinatorRuntime(
	FAssetDocumentSyncStateKey InKey,
	IAssetDocumentTypedSyncStateStore& InStore)
	: Key(MoveTemp(InKey))
	, Store(InStore)
{
}

FAssetDocumentSyncDecision FAssetDocumentSyncCoordinatorRuntime::Compare(
	const FAssetDocumentSyncComparisonInput& Input) const
{
	return ClassifyAssetDocumentSyncDirection(Input);
}

FAssetDocumentContractValidationResult FAssetDocumentSyncCoordinatorRuntime::AdvanceAfterDurableCompletion(
	const FAssetDocumentSyncEvidencePair& Installed,
	const FAssetDocumentDurableCompletionReceipt& Receipt)
{
	const FAssetDocumentContractValidationResult EvidenceValidation = ValidateComparablePair(Installed);
	if (!EvidenceValidation.bValid)
	{
		return EvidenceValidation;
	}
	const FAssetDocumentContractValidationResult ReceiptValidation = ValidateCompletionReceipt(Receipt);
	if (!ReceiptValidation.bValid)
	{
		return ReceiptValidation;
	}
	FAssetDocumentTypedSyncStateRecord Record;
	Record.LastDurable = Installed;
	Record.RequestId = Receipt.RequestId;
	Record.TransactionId = Receipt.TransactionId;
	Record.JournalId = Receipt.JournalId;
	return Store.Store(Key, Record);
}
