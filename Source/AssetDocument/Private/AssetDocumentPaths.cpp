// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPaths.h"

#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
namespace AssetDocumentPathsPlatform
{
AssetDocumentPaths::EPathLookup ResolveExistingPath(const FString& AbsolutePath, FString& OutRealPath)
{
	FString Native = AbsolutePath.Replace(TEXT("/"), TEXT("\\"));
	if (Native.Len() == 2 && Native[1] == TEXT(':'))
	{
		Native += TEXT("\\");
	}
	if (GetFileAttributesW(*Native) == INVALID_FILE_ATTRIBUTES)
	{
		const DWORD Error = GetLastError();
		return Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND || Error == ERROR_INVALID_NAME
			|| Error == ERROR_BAD_NETPATH || Error == ERROR_DIRECTORY
			? AssetDocumentPaths::EPathLookup::Missing : AssetDocumentPaths::EPathLookup::Unresolvable;
	}
	// The final path of an opened handle follows symbolic links and junctions and carries the stored case.
	const HANDLE File = CreateFileW(*Native, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (File == INVALID_HANDLE_VALUE)
	{
		return AssetDocumentPaths::EPathLookup::Unresolvable;
	}
	TArray<TCHAR> Buffer;
	Buffer.SetNumUninitialized(32768);
	const DWORD Length = GetFinalPathNameByHandleW(File, Buffer.GetData(), static_cast<DWORD>(Buffer.Num()),
		FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle(File);
	if (Length == 0 || Length >= static_cast<DWORD>(Buffer.Num()))
	{
		return AssetDocumentPaths::EPathLookup::Unresolvable;
	}
	FString Result(static_cast<int32>(Length), Buffer.GetData());
	if (Result.StartsWith(TEXT("\\\\?\\UNC\\")))
	{
		Result = TEXT("\\\\") + Result.RightChop(8);
	}
	else if (Result.StartsWith(TEXT("\\\\?\\")))
	{
		Result.RightChopInline(4);
	}
	OutRealPath = MoveTemp(Result);
	return AssetDocumentPaths::EPathLookup::Resolved;
}
}
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#if PLATFORM_APPLE
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace AssetDocumentPathsPlatform
{
#if PLATFORM_APPLE
// The engine creates files and folders on Apple platforms under their decomposed (NFD) names, and realpath returns
// those; package names and document text use precomposed (NFC) characters. A resolved path is returned precomposed,
// so a name with accents compares equal to the same name typed in a document (the file system accepts either form).
FString ToPrecomposed(const char* Utf8)
{
	FString Result(UTF8_TO_TCHAR(Utf8));
	CFMutableStringRef Text = CFStringCreateMutable(kCFAllocatorDefault, 0);
	if (Text == nullptr)
	{
		return Result;
	}
	CFStringAppendCString(Text, Utf8, kCFStringEncodingUTF8);
	CFStringNormalize(Text, kCFStringNormalizationFormC);
	const CFIndex Capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(Text), kCFStringEncodingUTF8) + 1;
	TArray<char> Buffer;
	Buffer.SetNumZeroed(static_cast<int32>(Capacity));
	if (CFStringGetCString(Text, Buffer.GetData(), Capacity, kCFStringEncodingUTF8))
	{
		Result = UTF8_TO_TCHAR(Buffer.GetData());
	}
	CFRelease(Text);
	return Result;
}
#else
FString ToPrecomposed(const char* Utf8)
{
	return FString(UTF8_TO_TCHAR(Utf8));
}
#endif

AssetDocumentPaths::EPathLookup ResolveExistingPath(const FString& AbsolutePath, FString& OutRealPath)
{
	const FTCHARToUTF8 Utf8(*AbsolutePath);
	char Resolved[PATH_MAX];
	if (realpath(Utf8.Get(), Resolved) != nullptr)
	{
		OutRealPath = ToPrecomposed(Resolved);
		return AssetDocumentPaths::EPathLookup::Resolved;
	}
	struct stat Entry;
	return lstat(Utf8.Get(), &Entry) == 0 ? AssetDocumentPaths::EPathLookup::Unresolvable : AssetDocumentPaths::EPathLookup::Missing;
}
}
#endif

namespace AssetDocumentPathsInternal
{
AssetDocumentPaths::FResolveExisting& ResolverOverride()
{
	static AssetDocumentPaths::FResolveExisting Override;
	return Override;
}

AssetDocumentPaths::EPathLookup Lookup(const FString& AbsolutePath, FString& OutRealPath)
{
	const AssetDocumentPaths::FResolveExisting& Override = ResolverOverride();
	return Override ? Override(AbsolutePath, OutRealPath) : AssetDocumentPathsPlatform::ResolveExistingPath(AbsolutePath, OutRealPath);
}

// "/" or "C:/": a path with no parent.
bool IsVolumeRoot(const FString& Path)
{
	return Path == TEXT("/") || (Path.Len() == 3 && Path[1] == TEXT(':') && Path[2] == TEXT('/'));
}

FString Normalized(const FString& Path)
{
	FString Result = Path;
	FPaths::NormalizeFilename(Result);
	while (Result.Len() > 1 && Result.EndsWith(TEXT("/")) && !IsVolumeRoot(Result))
	{
		Result.LeftChopInline(1);
	}
	return Result;
}

bool HasParentSegment(const FString& Path)
{
	return Path == TEXT("..") || Path.StartsWith(TEXT("../")) || Path.EndsWith(TEXT("/..")) || Path.Contains(TEXT("/../"));
}
}

namespace AssetDocumentPaths
{
FString ResolveRealPath(const FString& Path)
{
	using namespace AssetDocumentPathsInternal;
	if (Path.IsEmpty())
	{
		return FString();
	}
	const FString Full = Normalized(FPaths::ConvertRelativePathToFull(Path));
	if (HasParentSegment(Full))
	{
		return FString();
	}
	FString Existing = Full;
	FString Tail;
	for (;;)
	{
		FString Real;
		const EPathLookup Found = Lookup(Existing, Real);
		if (Found == EPathLookup::Resolved)
		{
			Real = Normalized(Real);
			if (Tail.IsEmpty())
			{
				return Real;
			}
			return Real.EndsWith(TEXT("/")) ? Real + Tail : Real + TEXT("/") + Tail;
		}
		int32 Slash = INDEX_NONE;
		if (Found == EPathLookup::Unresolvable || IsVolumeRoot(Existing) || !Existing.FindLastChar(TEXT('/'), Slash))
		{
			return FString();
		}
		const FString Name = Existing.Mid(Slash + 1);
		Tail = Tail.IsEmpty() ? Name : Name + TEXT("/") + Tail;
		Existing = Existing.Left(Slash);
		if (Existing.IsEmpty() || (Existing.Len() == 2 && Existing[1] == TEXT(':')))
		{
			Existing += TEXT("/");
		}
	}
}

bool ResolveWithin(const FString& Root, const FString& Candidate, FString& OutRealCandidate, FString& OutRelative)
{
	OutRealCandidate.Reset();
	const FString RealRoot = ResolveRealPath(Root);
	const FString RealCandidate = ResolveRealPath(Candidate);
	if (RealRoot.IsEmpty() || RealCandidate.IsEmpty())
	{
		return false;
	}
	if (RealCandidate.Equals(RealRoot, ESearchCase::CaseSensitive))
	{
		OutRelative.Reset();
		OutRealCandidate = RealCandidate;
		return true;
	}
	const FString Prefix = RealRoot.EndsWith(TEXT("/")) ? RealRoot : RealRoot + TEXT("/");
	if (!RealCandidate.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	OutRelative = RealCandidate.RightChop(Prefix.Len());
	OutRealCandidate = RealCandidate;
	return true;
}

bool MakeRelativeWithin(const FString& Root, const FString& Candidate, FString& OutRelative)
{
	FString RealCandidate;
	return ResolveWithin(Root, Candidate, RealCandidate, OutRelative);
}

bool IsWithin(const FString& Root, const FString& Candidate)
{
	FString Relative;
	return MakeRelativeWithin(Root, Candidate, Relative);
}

bool ResolveForAccess(const FString& Root, const FString& Path, FString& OutRealPath)
{
	FString Relative;
	return ResolveWithin(Root, Path, OutRealPath, Relative);
}

FScopedResolverOverride::FScopedResolverOverride(FResolveExisting Resolver)
	: Previous(MoveTemp(AssetDocumentPathsInternal::ResolverOverride()))
{
	AssetDocumentPathsInternal::ResolverOverride() = MoveTemp(Resolver);
}

FScopedResolverOverride::~FScopedResolverOverride()
{
	AssetDocumentPathsInternal::ResolverOverride() = MoveTemp(Previous);
}
}
