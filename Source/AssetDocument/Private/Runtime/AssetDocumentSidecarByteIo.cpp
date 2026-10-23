// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentSidecarByteIo.h"

#include "AssetDocumentAtomicFile.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{

FAssetDocumentContractValidationResult MakeSidecarResult(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Persistence/Sidecar");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

FAssetDocumentContractValidationResult MakeValidSidecarResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

constexpr uint32 Sha256RoundConstants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32 RotateRight(uint32 Value, uint32 Bits)
{
	return (Value >> Bits) | (Value << (32 - Bits));
}

void ExpandSha256Block(const uint8* Block, uint32 (&Words)[64])
{
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const uint8* Cursor = Block + Index * 4;
		Words[Index] = (uint32(Cursor[0]) << 24) | (uint32(Cursor[1]) << 16) | (uint32(Cursor[2]) << 8) | Cursor[3];
	}
	for (int32 Index = 16; Index < 64; ++Index)
	{
		const uint32 S0 = RotateRight(Words[Index - 15], 7) ^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
		const uint32 S1 = RotateRight(Words[Index - 2], 17) ^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
		Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
	}
}

void CompressSha256Block(const uint8* Block, uint32 (&Hash)[8])
{
	uint32 Words[64];
	ExpandSha256Block(Block, Words);
	uint32 A = Hash[0], B = Hash[1], C = Hash[2], D = Hash[3];
	uint32 E = Hash[4], F = Hash[5], G = Hash[6], H = Hash[7];
	for (int32 Index = 0; Index < 64; ++Index)
	{
		const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
		const uint32 Temporary1 = H + S1 + ((E & F) ^ (~E & G)) + Sha256RoundConstants[Index] + Words[Index];
		const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
		const uint32 Temporary2 = S0 + ((A & B) ^ (A & C) ^ (B & C));
		H = G; G = F; F = E; E = D + Temporary1;
		D = C; C = B; B = A; A = Temporary1 + Temporary2;
	}
	Hash[0] += A; Hash[1] += B; Hash[2] += C; Hash[3] += D;
	Hash[4] += E; Hash[5] += F; Hash[6] += G; Hash[7] += H;
}

TArray<uint8> PadSha256Input(TArrayView64<const uint8> Bytes)
{
	TArray<uint8> Padded;
	Padded.Append(Bytes.GetData(), Bytes.Num());
	Padded.Add(0x80);
	while ((Padded.Num() % 64) != 56)
	{
		Padded.Add(0);
	}
	const uint64 BitCount = static_cast<uint64>(Bytes.Num()) * 8;
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Padded.Add(static_cast<uint8>(BitCount >> Shift));
	}
	return Padded;
}

bool LoadAndValidate(const FString& Path, const FString& ExpectedDigest, TArray64<uint8>& OutBytes)
{
	return FFileHelper::LoadFileToArray(OutBytes, *Path)
		&& ComputeAssetDocumentContentDigest(OutBytes) == ExpectedDigest;
}

bool WriteExact(const FString& Path, TArrayView64<const uint8> Bytes)
{
#if PLATFORM_WINDOWS
	FAssetDocumentAtomicFile::ReleaseLoadedPackageFileHandles(Path);
#endif
	FString Error;
	return FAssetDocumentAtomicFile::WriteBytesAtomically(Path, Bytes, Error);
}

bool IsSidecarStageInputValid(const FAssetDocumentPersistencePlan& Plan)
{
	return !Plan.StagingRoot.IsEmpty() && !Plan.TargetSidecarPath.IsEmpty()
		&& !Plan.Sidecar.Utf8Bytes.IsEmpty();
}

FAssetDocumentContractValidationResult RestoreAbsent(const FAssetDocumentJournalArtifact& Artifact)
{
	bool bRemoved = true;
	if (IFileManager::Get().FileExists(*Artifact.TargetPath))
	{
#if PLATFORM_WINDOWS
		FAssetDocumentAtomicFile::ReleaseLoadedPackageFileHandles(Artifact.TargetPath);
#endif
		bRemoved = IFileManager::Get().Delete(*Artifact.TargetPath, false, true, true);
	}
	FString Error;
	if (!bRemoved || !FAssetDocumentAtomicFile::FlushParentDirectory(Artifact.TargetPath, Error))
	{
		return MakeSidecarResult(TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Absent sidecar baseline could not be restored durably."));
	}
	return MakeValidSidecarResult();
}

FAssetDocumentContractValidationResult RestoreExisting(const FAssetDocumentJournalArtifact& Artifact)
{
	TArray64<uint8> Bytes;
	if (!LoadAndValidate(Artifact.BackupPath, Artifact.PreimageContentDigest, Bytes))
	{
		return MakeSidecarResult(TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Exact sidecar baseline could not be restored."));
	}
	if (!WriteExact(Artifact.TargetPath, Bytes))
	{
		return MakeSidecarResult(TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Exact sidecar baseline could not be restored."));
	}
	return MakeValidSidecarResult();
}
}

FString ComputeAssetDocumentContentDigest(TArrayView64<const uint8> Bytes)
{
	uint32 Hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	const TArray<uint8> Padded = PadSha256Input(Bytes);
	for (int64 Offset = 0; Offset < Padded.Num(); Offset += 64)
	{
		CompressSha256Block(Padded.GetData() + Offset, Hash);
	}
	FString Hex;
	for (uint32 Word : Hash)
	{
		Hex += FString::Printf(TEXT("%08x"), Word);
	}
	return TEXT("sha256:") + Hex;
}

FAssetDocumentContractValidationResult FAssetDocumentSidecarByteIo::Stage(
	const FAssetDocumentPersistencePlan& Plan,
	FAssetDocumentByteArtifact& OutArtifact)
{
	if (!IsSidecarStageInputValid(Plan))
	{
		return MakeSidecarResult(TEXT("Persistence.StageSidecarFailed"), TEXT("Sidecar staging input is incomplete."));
	}
	const FString StagedPath = FPaths::Combine(Plan.StagingRoot, TEXT("sidecar.stage"));
	if (!IFileManager::Get().MakeDirectory(*Plan.StagingRoot, true)
		|| !WriteExact(StagedPath, Plan.Sidecar.Utf8Bytes))
	{
		return MakeSidecarResult(TEXT("Persistence.StageSidecarFailed"), TEXT("Sidecar staging write failed."));
	}
	OutArtifact.StagedPath = StagedPath;
	OutArtifact.TargetPath = FPaths::ConvertRelativePathToFull(Plan.TargetSidecarPath);
	OutArtifact.ContentDigest = ComputeAssetDocumentContentDigest(Plan.Sidecar.Utf8Bytes);
	OutArtifact.bTargetExisted = IFileManager::Get().FileExists(*OutArtifact.TargetPath);
	return MakeValidSidecarResult();
}

FAssetDocumentContractValidationResult FAssetDocumentSidecarByteIo::CaptureInstalledBaseline(
	const FAssetDocumentByteArtifact& Artifact,
	const FString& BackupPath,
	FAssetDocumentJournalArtifact& OutJournalArtifact)
{
	OutJournalArtifact.TargetPath = Artifact.TargetPath;
	OutJournalArtifact.BackupPath = BackupPath;
	OutJournalArtifact.InstalledContentDigest = Artifact.ContentDigest;
	OutJournalArtifact.bTargetExisted = IFileManager::Get().FileExists(*Artifact.TargetPath);
	TArray64<uint8> Baseline;
	if (!OutJournalArtifact.bTargetExisted)
	{
		return MakeValidSidecarResult();
	}
	if (!FFileHelper::LoadFileToArray(Baseline, *Artifact.TargetPath) || !WriteExact(BackupPath, Baseline))
	{
		return MakeSidecarResult(TEXT("Persistence.PrepareJournalFailed"), TEXT("Sidecar baseline capture failed."));
	}
	OutJournalArtifact.PreimageContentDigest = ComputeAssetDocumentContentDigest(Baseline);
	return MakeValidSidecarResult();
}

void FAssetDocumentSidecarByteIo::CleanupStaged(
	const FAssetDocumentByteArtifact& Artifact,
	FStringView StagingRoot)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FString(StagingRoot));
	const FString Path = FPaths::ConvertRelativePathToFull(Artifact.StagedPath);
	if (!Artifact.StagedPath.IsEmpty() && FPaths::IsUnderDirectory(Path, Root))
	{
		IFileManager::Get().Delete(*Path, false, true, true);
	}
}

FAssetDocumentContractValidationResult FAssetDocumentSidecarByteIo::Install(
	const FAssetDocumentByteArtifact& Artifact)
{
	TArray64<uint8> Bytes;
	if (!LoadAndValidate(Artifact.StagedPath, Artifact.ContentDigest, Bytes)
		|| !WriteExact(Artifact.TargetPath, Bytes))
	{
		return MakeSidecarResult(TEXT("Persistence.AtomicInstallFailed"), TEXT("Sidecar staged bytes were invalid or could not be installed."));
	}
	return MakeValidSidecarResult();
}

FAssetDocumentContractValidationResult FAssetDocumentSidecarByteIo::Restore(
	const FAssetDocumentJournalArtifact& Artifact)
{
	if (!Artifact.bTargetExisted)
	{
		return RestoreAbsent(Artifact);
	}
	return RestoreExisting(Artifact);
}
