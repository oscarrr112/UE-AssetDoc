// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

#if PLATFORM_MAC
#include <membership.h>
#include <sys/acl.h>
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <aclapi.h>
#include <sddl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace AssetDocumentUserOnlyFileTests
{
#if PLATFORM_MAC
/** Whether the file or folder carries at least one extended ACL entry. */
inline bool HasExtendedAcl(const FString& Path)
{
	acl_t Acl = acl_get_file(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Path)), ACL_TYPE_EXTENDED);
	if (Acl == nullptr)
	{
		return false;
	}
	acl_entry_t Entry = nullptr;
	const bool bHasEntry = acl_get_entry(Acl, ACL_FIRST_ENTRY, &Entry) == 0;
	acl_free(Acl);
	return bHasEntry;
}

/**
 * Gives the group "everyone" read access to Path through an extended ACL entry; with bInheritedByFiles (a folder),
 * every file created in it afterwards inherits the entry. OutWhy says what failed.
 */
inline bool AllowEveryoneToRead(const FString& Path, bool bInheritedByFiles, FString& OutWhy)
{
	uuid_t Everyone;
	if (mbr_gid_to_uuid(12, Everyone) != 0)
	{
		OutWhy = TEXT("the UUID of the group everyone cannot be read");
		return false;
	}
	acl_t Acl = acl_init(1);
	acl_entry_t Entry = nullptr;
	acl_permset_t Permissions = nullptr;
	acl_flagset_t Flags = nullptr;
	bool bOk = Acl != nullptr && acl_create_entry(&Acl, &Entry) == 0 && acl_set_tag_type(Entry, ACL_EXTENDED_ALLOW) == 0
		&& acl_set_qualifier(Entry, Everyone) == 0 && acl_get_permset(Entry, &Permissions) == 0 && acl_add_perm(Permissions, ACL_READ_DATA) == 0
		&& acl_set_permset(Entry, Permissions) == 0;
	if (bOk && bInheritedByFiles)
	{
		bOk = acl_get_flagset_np(Entry, &Flags) == 0 && acl_add_flag_np(Flags, ACL_ENTRY_FILE_INHERIT) == 0 && acl_set_flagset_np(Entry, Flags) == 0;
	}
	bOk = bOk && acl_set_file(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Path)), ACL_TYPE_EXTENDED, Acl) == 0;
	if (!bOk)
	{
		OutWhy = FString::Printf(TEXT("setting the ACL failed (errno %d)"), errno);
	}
	if (Acl != nullptr)
	{
		acl_free(Acl);
	}
	return bOk;
}
#endif

/**
 * Whether only the current user can read the file, read from the file system on its own (not with the code that
 * wrote it). Mac and Linux: no group or other permission bits, and on the Mac no extended ACL entry. Windows: the DACL is protected (inheritance off) and
 * every allowed entry is the current user or SYSTEM, the user among them. OutWhy says what is wrong.
 */
inline bool IsReadableByCurrentUserOnly(const FString& Path, FString& OutWhy)
{
	const FString Full = FPaths::ConvertRelativePathToFull(Path);
#if PLATFORM_MAC || PLATFORM_LINUX
	struct stat Status;
	if (stat(TCHAR_TO_UTF8(*Full), &Status) != 0)
	{
		OutWhy = TEXT("the file cannot be examined");
		return false;
	}
	if ((Status.st_mode & 077) != 0)
	{
		const uint32 Mode = static_cast<uint32>(Status.st_mode & 0777);
		OutWhy = FString::Printf(TEXT("mode 0%u%u%u gives the group or others access"), (Mode >> 6) & 7, (Mode >> 3) & 7, Mode & 7);
		return false;
	}
#if PLATFORM_MAC
	if (HasExtendedAcl(Full))
	{
		OutWhy = TEXT("an extended ACL entry gives other accounts access");
		return false;
	}
#endif
	return true;
#elif PLATFORM_WINDOWS
	PACL Dacl = nullptr;
	PSECURITY_DESCRIPTOR Descriptor = nullptr;
	const DWORD Read = ::GetNamedSecurityInfoW(*Full.Replace(TEXT("/"), TEXT("\\")), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
		&Dacl, nullptr, &Descriptor);
	if (Read != ERROR_SUCCESS)
	{
		OutWhy = FString::Printf(TEXT("GetNamedSecurityInfo failed (error %u)"), Read);
		return false;
	}
	HANDLE Token = nullptr;
	TArray<uint8> UserInfo;
	if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &Token))
	{
		DWORD Size = 0;
		::GetTokenInformation(Token, ::TokenUser, nullptr, 0, &Size);
		UserInfo.SetNumZeroed(static_cast<int32>(Size));
		if (Size == 0 || !::GetTokenInformation(Token, ::TokenUser, UserInfo.GetData(), Size, &Size))
		{
			UserInfo.Reset();
		}
		::CloseHandle(Token);
	}
	uint8 SystemSid[SECURITY_MAX_SID_SIZE] = {};
	DWORD SystemSize = SECURITY_MAX_SID_SIZE;
	const bool bSystemSid = ::CreateWellKnownSid(WinLocalSystemSid, nullptr, SystemSid, &SystemSize) != 0;
	SECURITY_DESCRIPTOR_CONTROL Control = 0;
	DWORD Revision = 0;
	bool bOk = true;
	if (UserInfo.IsEmpty() || !bSystemSid)
	{
		OutWhy = TEXT("the current user's or SYSTEM's SID cannot be read");
		bOk = false;
	}
	else if (Dacl == nullptr)
	{
		OutWhy = TEXT("the file has no DACL (everyone has access)");
		bOk = false;
	}
	else if (!::GetSecurityDescriptorControl(Descriptor, &Control, &Revision) || (Control & SE_DACL_PROTECTED) == 0)
	{
		OutWhy = TEXT("the DACL is not protected: it inherits entries from the folder");
		bOk = false;
	}
	else
	{
		PSID UserSid = reinterpret_cast<TOKEN_USER*>(UserInfo.GetData())->User.Sid;
		bool bUserAllowed = false;
		for (DWORD Index = 0; bOk && Index < Dacl->AceCount; ++Index)
		{
			void* Ace = nullptr;
			if (!::GetAce(Dacl, Index, &Ace) || Ace == nullptr)
			{
				OutWhy = TEXT("an entry of the DACL cannot be read");
				bOk = false;
				break;
			}
			const ACE_HEADER* Header = static_cast<const ACE_HEADER*>(Ace);
			if (Header->AceType == ACCESS_DENIED_ACE_TYPE)
			{
				continue;
			}
			if (Header->AceType != ACCESS_ALLOWED_ACE_TYPE)
			{
				OutWhy = FString::Printf(TEXT("the DACL has an entry of type %u"), static_cast<uint32>(Header->AceType));
				bOk = false;
				break;
			}
			PSID Sid = static_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(Ace)->SidStart);
			if (::EqualSid(Sid, UserSid))
			{
				bUserAllowed = true;
			}
			else if (!::EqualSid(Sid, SystemSid))
			{
				LPWSTR SidText = nullptr;
				::ConvertSidToStringSidW(Sid, &SidText);
				OutWhy = FString::Printf(TEXT("the DACL allows %s"), SidText != nullptr ? SidText : TEXT("another SID"));
				if (SidText != nullptr)
				{
					::LocalFree(SidText);
				}
				bOk = false;
			}
		}
		if (bOk && !bUserAllowed)
		{
			OutWhy = TEXT("the DACL does not allow the current user");
			bOk = false;
		}
	}
	::LocalFree(Descriptor);
	return bOk;
#else
	OutWhy = TEXT("no check on this platform");
	return false;
#endif
}
}
