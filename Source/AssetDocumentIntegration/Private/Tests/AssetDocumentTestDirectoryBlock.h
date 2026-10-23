// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <aclapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// Failure-injection helper shared by the persistence rollback tests: makes a
// directory reject creation and deletion of its children (POSIX: read-only
// directory mode). On Windows the read-only attribute of a directory has no such
// effect, so a scratch directory under Saved/ is swapped for a read-only file placeholder (asset folders get the ACE only) and any
// other directory gets a DENY ACE for Everyone on its DACL (see SetDirectoryReadOnly).
// Pass bBlocked=false to restore. Returns true when the requested state was applied.
//
// NOTE: an identical copy lives in AssetDocumentEditor/Private/Tests; keep in sync.
namespace AssetDocumentTestDirectoryBlock
{
#if PLATFORM_WINDOWS
inline bool SetWindowsDenyAce(const FString& Directory, bool bBlocked)
{
	constexpr DWORD DeniedMask = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD;
	BYTE EveryoneSid[SECURITY_MAX_SID_SIZE];
	DWORD SidSize = sizeof(EveryoneSid);
	if (!CreateWellKnownSid(WinWorldSid, nullptr, EveryoneSid, &SidSize)) { return false; }
	const FString Native = Directory.Replace(TEXT("/"), TEXT("\\"));
	PACL OldDacl = nullptr;
	PSECURITY_DESCRIPTOR Descriptor = nullptr;
	if (GetNamedSecurityInfoW(*Native, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &OldDacl, nullptr, &Descriptor) != ERROR_SUCCESS)
	{
		return false;
	}
	bool bOk = false;
	if (bBlocked)
	{
		EXPLICIT_ACCESS_W Access;
		FMemory::Memzero(Access);
		Access.grfAccessPermissions = DeniedMask;
		Access.grfAccessMode = DENY_ACCESS;
		Access.grfInheritance = NO_INHERITANCE;
		Access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		Access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
		Access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(EveryoneSid);
		PACL NewDacl = nullptr;
		if (SetEntriesInAclW(1, &Access, OldDacl, &NewDacl) == ERROR_SUCCESS)
		{
			bOk = SetNamedSecurityInfoW(const_cast<LPWSTR>(*Native), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, NewDacl, nullptr) == ERROR_SUCCESS;
			LocalFree(NewDacl);
		}
	}
	else
	{
		ACL_SIZE_INFORMATION Info = {};
		bOk = OldDacl != nullptr && GetAclInformation(OldDacl, &Info, sizeof(Info), AclSizeInformation);
		for (int32 Index = bOk ? static_cast<int32>(Info.AceCount) - 1 : -1; Index >= 0; --Index)
		{
			void* Ace = nullptr;
			if (GetAce(OldDacl, Index, &Ace) && static_cast<ACE_HEADER*>(Ace)->AceType == ACCESS_DENIED_ACE_TYPE)
			{
				const ACCESS_DENIED_ACE* Denied = static_cast<ACCESS_DENIED_ACE*>(Ace);
				if (Denied->Mask == DeniedMask && EqualSid(const_cast<PSID>(reinterpret_cast<const void*>(&Denied->SidStart)), EveryoneSid))
				{
					DeleteAce(OldDacl, Index);
				}
			}
		}
		if (bOk)
		{
			bOk = SetNamedSecurityInfoW(const_cast<LPWSTR>(*Native), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, OldDacl, nullptr) == ERROR_SUCCESS;
		}
	}
	LocalFree(Descriptor);
	return bOk;
}
#endif

// Runtime scratch under Saved/ (staging, journals) may be moved aside; asset folders
// under Content/ hold the packages the failure must leave intact.
inline bool IsScratchDirectory(const FString& Directory)
{
	return FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Directory),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
}

inline bool SetDirectoryReadOnly(IPlatformFile& Files, const TCHAR* Directory, bool bBlocked)
{
#if PLATFORM_WINDOWS
	// A DENY ACE blocks creating files in the directory, but not creating subdirectories
	// when the process token has SeRestorePrivilege enabled (typical for CI/ssh sessions),
	// and the persistence staging code creates a fresh subdirectory per request. A scratch
	// directory (under Saved/) is therefore moved aside and replaced by a read-only regular file of the
	// same name, which rejects any child creation under every token; restore moves it back.
	// Asset folders keep their content and get the ACE only.
	const FString Aside = FString(Directory) + TEXT(".adtest-aside");
	if (bBlocked)
	{
		if (Files.FileExists(Directory))
		{
			return true;
		}
		if (Files.DirectoryExists(Directory) && IsScratchDirectory(Directory))
		{
			if (Files.DirectoryExists(*Aside))
			{
				Files.DeleteDirectoryRecursively(*Aside);
			}
			if (!Files.MoveFile(*Aside, Directory))
			{
				return false;
			}
			if (TUniquePtr<IFileHandle> Placeholder(Files.OpenWrite(Directory)); Placeholder.IsValid())
			{
				Placeholder.Reset();
				return Files.SetReadOnly(Directory, true);
			}
			Files.MoveFile(Directory, *Aside);
			return false;
		}
		return Files.SetReadOnly(Directory, true) && SetWindowsDenyAce(Directory, true);
	}
	if (Files.FileExists(Directory))
	{
		if (!Files.SetReadOnly(Directory, false) || !Files.DeleteFile(Directory))
		{
			return false;
		}
		return Files.DirectoryExists(*Aside) ? Files.MoveFile(Directory, *Aside) : Files.CreateDirectory(Directory);
	}
	const bool bAclRestored = SetWindowsDenyAce(Directory, false);
	return Files.SetReadOnly(Directory, false) && bAclRestored;
#else
	return Files.SetReadOnly(Directory, bBlocked);
#endif
}
}
