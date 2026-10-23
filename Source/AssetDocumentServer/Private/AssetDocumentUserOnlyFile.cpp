// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUserOnlyFile.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if PLATFORM_MAC
#include <sys/acl.h>
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <aclapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace AssetDocumentUserOnlyFile
{
#if PLATFORM_WINDOWS
namespace UserOnlyFileWindows
{
FString DescribeError(const TCHAR* Call, uint32 Code)
{
	TCHAR Buffer[512] = {};
	FPlatformMisc::GetSystemErrorMessage(Buffer, UE_ARRAY_COUNT(Buffer), static_cast<int32>(Code));
	return FString::Printf(TEXT("%s failed (error %u: %s)"), Call, Code, *FString(Buffer).TrimStartAndEnd());
}

// The SID of the user this process runs as.
bool GetCurrentUserSid(TArray<uint8>& OutSid, FString& OutError)
{
	HANDLE Token = nullptr;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &Token))
	{
		OutError = DescribeError(TEXT("OpenProcessToken"), ::GetLastError());
		return false;
	}
	DWORD Size = 0;
	::GetTokenInformation(Token, TokenUser, nullptr, 0, &Size);
	TArray<uint8> Buffer;
	Buffer.SetNumZeroed(static_cast<int32>(Size));
	const bool bRead = Size > 0 && ::GetTokenInformation(Token, TokenUser, Buffer.GetData(), Size, &Size);
	const DWORD ReadError = ::GetLastError();
	::CloseHandle(Token);
	if (!bRead)
	{
		OutError = DescribeError(TEXT("GetTokenInformation(TokenUser)"), ReadError);
		return false;
	}
	PSID UserSid = reinterpret_cast<const TOKEN_USER*>(Buffer.GetData())->User.Sid;
	const DWORD SidLength = ::GetLengthSid(UserSid);
	OutSid.SetNumZeroed(static_cast<int32>(SidLength));
	if (!::CopySid(SidLength, OutSid.GetData(), UserSid))
	{
		OutError = DescribeError(TEXT("CopySid"), ::GetLastError());
		return false;
	}
	return true;
}

bool GetSystemSid(TArray<uint8>& OutSid, FString& OutError)
{
	DWORD Size = SECURITY_MAX_SID_SIZE;
	OutSid.SetNumZeroed(static_cast<int32>(Size));
	if (!::CreateWellKnownSid(WinLocalSystemSid, nullptr, OutSid.GetData(), &Size))
	{
		OutError = DescribeError(TEXT("CreateWellKnownSid(LocalSystem)"), ::GetLastError());
		return false;
	}
	OutSid.SetNum(static_cast<int32>(Size));
	return true;
}

// The DACL a protected security descriptor carries: full access for the user and SYSTEM, nothing else.
bool BuildUserOnlyAcl(TArray<uint8>& UserSid, TArray<uint8>& SystemSid, TArray<uint8>& OutAcl, FString& OutError)
{
	const DWORD AclSize = sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) + ::GetLengthSid(UserSid.GetData())
		+ ::GetLengthSid(SystemSid.GetData());
	OutAcl.SetNumZeroed(static_cast<int32>(AclSize));
	PACL Acl = reinterpret_cast<PACL>(OutAcl.GetData());
	if (!::InitializeAcl(Acl, AclSize, ACL_REVISION))
	{
		OutError = DescribeError(TEXT("InitializeAcl"), ::GetLastError());
		return false;
	}
	if (!::AddAccessAllowedAce(Acl, ACL_REVISION, FILE_ALL_ACCESS, UserSid.GetData())
		|| !::AddAccessAllowedAce(Acl, ACL_REVISION, FILE_ALL_ACCESS, SystemSid.GetData()))
	{
		OutError = DescribeError(TEXT("AddAccessAllowedAce"), ::GetLastError());
		return false;
	}
	return true;
}

// Whether the file's DACL is protected (inheritance off) and allows only the user and SYSTEM.
bool CheckUserOnly(HANDLE File, TArray<uint8>& UserSid, TArray<uint8>& SystemSid, FString& OutError)
{
	PACL Dacl = nullptr;
	PSECURITY_DESCRIPTOR Descriptor = nullptr;
	const DWORD Result = ::GetSecurityInfo(File, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Dacl, nullptr, &Descriptor);
	if (Result != ERROR_SUCCESS)
	{
		OutError = DescribeError(TEXT("GetSecurityInfo"), Result);
		return false;
	}
	bool bUserOnly = Dacl != nullptr;
	SECURITY_DESCRIPTOR_CONTROL Control = 0;
	DWORD Revision = 0;
	bUserOnly = bUserOnly && ::GetSecurityDescriptorControl(Descriptor, &Control, &Revision) && (Control & SE_DACL_PROTECTED) != 0;
	for (DWORD Index = 0; bUserOnly && Index < Dacl->AceCount; ++Index)
	{
		void* Ace = nullptr;
		if (!::GetAce(Dacl, Index, &Ace) || Ace == nullptr)
		{
			bUserOnly = false;
			break;
		}
		const ACE_HEADER* Header = static_cast<const ACE_HEADER*>(Ace);
		if (Header->AceType == ACCESS_DENIED_ACE_TYPE)
		{
			continue;
		}
		if (Header->AceType != ACCESS_ALLOWED_ACE_TYPE)
		{
			bUserOnly = false;
			break;
		}
		PSID Sid = reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(Ace)->SidStart);
		bUserOnly = ::EqualSid(Sid, UserSid.GetData()) || ::EqualSid(Sid, SystemSid.GetData());
	}
	::LocalFree(Descriptor);
	if (!bUserOnly)
	{
		OutError = TEXT("the file's access list still allows other users or inherits entries from its folder");
	}
	return bUserOnly;
}

bool Save(const FString& Text, const FString& Path, FString& OutError)
{
	TArray<uint8> UserSid;
	TArray<uint8> SystemSid;
	TArray<uint8> AclBytes;
	if (!GetCurrentUserSid(UserSid, OutError) || !GetSystemSid(SystemSid, OutError) || !BuildUserOnlyAcl(UserSid, SystemSid, AclBytes, OutError))
	{
		return false;
	}
	SECURITY_DESCRIPTOR Descriptor;
	if (!::InitializeSecurityDescriptor(&Descriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !::SetSecurityDescriptorDacl(&Descriptor, true, reinterpret_cast<PACL>(AclBytes.GetData()), false)
		|| !::SetSecurityDescriptorControl(&Descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
	{
		OutError = DescribeError(TEXT("building the security descriptor"), ::GetLastError());
		return false;
	}
	SECURITY_ATTRIBUTES Attributes;
	Attributes.nLength = sizeof(Attributes);
	Attributes.lpSecurityDescriptor = &Descriptor;
	Attributes.bInheritHandle = false;

	// A new file gets the descriptor at creation (an existing one would keep its own), so write a temporary file
	// and move it over the target; a move on the same volume keeps the descriptor.
	const FString Full = FPaths::ConvertRelativePathToFull(Path).Replace(TEXT("/"), TEXT("\\"));
	const FString Temporary = Full + TEXT(".tmp");
	::DeleteFileW(*Temporary);
	HANDLE File = ::CreateFileW(*Temporary, GENERIC_WRITE | READ_CONTROL, 0, &Attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (File == INVALID_HANDLE_VALUE)
	{
		OutError = DescribeError(TEXT("CreateFile"), ::GetLastError());
		return false;
	}
	const FTCHARToUTF8 Utf8(*Text);
	DWORD Written = 0;
	const bool bWritten = ::WriteFile(File, Utf8.Get(), static_cast<DWORD>(Utf8.Length()), &Written, nullptr) && Written == static_cast<DWORD>(Utf8.Length());
	const DWORD WriteError = ::GetLastError();
	const bool bUserOnly = bWritten && CheckUserOnly(File, UserSid, SystemSid, OutError);
	::CloseHandle(File);
	if (!bWritten)
	{
		OutError = DescribeError(TEXT("WriteFile"), WriteError);
	}
	if (!bUserOnly)
	{
		::DeleteFileW(*Temporary);
		return false;
	}
	if (!::MoveFileExW(*Temporary, *Full, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		OutError = DescribeError(TEXT("MoveFileEx"), ::GetLastError());
		::DeleteFileW(*Temporary);
		return false;
	}
	// The moved file keeps the temporary file's descriptor; check the file that now holds the text.
	HANDLE Moved = ::CreateFileW(*Full, READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (Moved == INVALID_HANDLE_VALUE)
	{
		OutError = DescribeError(TEXT("CreateFile (check)"), ::GetLastError());
		return false;
	}
	const bool bMovedUserOnly = CheckUserOnly(Moved, UserSid, SystemSid, OutError);
	::CloseHandle(Moved);
	return bMovedUserOnly;
}
} // namespace UserOnlyFileWindows
#endif

#if PLATFORM_MAC
namespace UserOnlyFileMac
{
// A file system without extended ACLs answers ENOTSUP: no entry can grant access there.
bool IsUnsupported(int Error)
{
	return Error == ENOTSUP || Error == EOPNOTSUPP;
}

// Removes every extended ACL entry of the open file (explicit, or inherited from its folder), which can grant other
// accounts access the mode bits do not show, then reads the ACL back to confirm none is left.
bool ClearExtendedAcl(int Descriptor, FString& OutError)
{
	acl_t Empty = acl_init(0);
	if (Empty == nullptr)
	{
		OutError = FString::Printf(TEXT("acl_init failed (errno %d)"), errno);
		return false;
	}
	const int SetResult = acl_set_fd_np(Descriptor, Empty, ACL_TYPE_EXTENDED);
	const int SetError = errno;
	acl_free(Empty);
	if (SetResult != 0 && !IsUnsupported(SetError))
	{
		OutError = FString::Printf(TEXT("clearing the extended ACL failed (errno %d)"), SetError);
		return false;
	}
	acl_t Left = acl_get_fd_np(Descriptor, ACL_TYPE_EXTENDED);
	if (Left == nullptr)
	{
		const int GetError = errno;
		if (GetError == ENOENT || IsUnsupported(GetError))
		{
			return true;
		}
		OutError = FString::Printf(TEXT("reading the extended ACL failed (errno %d)"), GetError);
		return false;
	}
	acl_entry_t Entry = nullptr;
	const bool bEntryLeft = acl_get_entry(Left, ACL_FIRST_ENTRY, &Entry) == 0;
	acl_free(Left);
	if (bEntryLeft)
	{
		OutError = TEXT("an extended ACL entry is still on the file after clearing it");
		return false;
	}
	return true;
}
} // namespace UserOnlyFileMac
#endif

#if PLATFORM_MAC || PLATFORM_LINUX
namespace UserOnlyFilePosix
{
// Restricts the open file or folder to the current user: Mode (0600 or 0700) and, on the Mac, no extended ACL entry.
bool RestrictToUser(int Descriptor, mode_t Mode, FString& OutError)
{
	if (fchmod(Descriptor, Mode) != 0)
	{
		OutError = FString::Printf(TEXT("fchmod failed (errno %d)"), errno);
		return false;
	}
#if PLATFORM_MAC
	return UserOnlyFileMac::ClearExtendedAcl(Descriptor, OutError);
#else
	return true;
#endif
}

// Writes Text to a new file and renames it over Path. The file is a new inode, so a reader that opened the old
// file keeps the old text and never sees the new one. It is created with O_EXCL in a new folder next to Path that is
// restricted to the current user before anything is created in it: the file inherits no entry from Path's folder,
// and no other account can open it between its creation and its own restriction.
bool Save(const FString& Text, const FString& Path, FString& OutError)
{
	const FString Full = FPaths::ConvertRelativePathToFull(Path);
	const FString Staging = FString::Printf(TEXT("%s.%s.tmp"), *Full, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FTCHARToUTF8 StagingPath(*Staging);
	if (mkdir(StagingPath.Get(), S_IRWXU) != 0)
	{
		OutError = FString::Printf(TEXT("mkdir of the staging folder failed (errno %d)"), errno);
		return false;
	}
	const int Folder = open(StagingPath.Get(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (Folder < 0)
	{
		OutError = FString::Printf(TEXT("open of the staging folder failed (errno %d)"), errno);
		rmdir(StagingPath.Get());
		return false;
	}
	static const char* const FileName = "service";
	bool bCreated = false;
	bool bSaved = RestrictToUser(Folder, S_IRWXU, OutError);
	if (bSaved)
	{
		const int Descriptor = openat(Folder, FileName, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
		bCreated = Descriptor >= 0;
		if (!bCreated)
		{
			OutError = FString::Printf(TEXT("open of the new file failed (errno %d)"), errno);
			bSaved = false;
		}
		else
		{
			const FTCHARToUTF8 Utf8(*Text);
			bSaved = RestrictToUser(Descriptor, S_IRUSR | S_IWUSR, OutError);
			if (bSaved)
			{
				const ssize_t Written = write(Descriptor, Utf8.Get(), Utf8.Length());
				bSaved = Written == Utf8.Length();
				if (!bSaved)
				{
					OutError = FString::Printf(TEXT("write failed (errno %d)"), errno);
				}
			}
			if (close(Descriptor) != 0 && bSaved)
			{
				OutError = FString::Printf(TEXT("close failed (errno %d)"), errno);
				bSaved = false;
			}
		}
	}
	if (bSaved && renameat(Folder, FileName, AT_FDCWD, TCHAR_TO_UTF8(*Full)) != 0)
	{
		OutError = FString::Printf(TEXT("rename over the file failed (errno %d)"), errno);
		bSaved = false;
	}
	if (!bSaved && bCreated)
	{
		unlinkat(Folder, FileName, 0);
	}
	close(Folder);
	rmdir(StagingPath.Get());
	return bSaved;
}
} // namespace UserOnlyFilePosix
#endif

bool Save(const FString& Text, const FString& Path, FString& OutError)
{
	OutError.Reset();
	bool bSaved = false;
#if PLATFORM_MAC || PLATFORM_LINUX
	bSaved = UserOnlyFilePosix::Save(Text, Path, OutError);
#elif PLATFORM_WINDOWS
	bSaved = UserOnlyFileWindows::Save(Text, Path, OutError);
#else
	OutError = TEXT("this platform has no way to make the file readable by the current user only");
#endif
	if (!bSaved)
	{
		// Never leave a file whose access is not restricted (or an older copy of it): the reader then reports the
		// missing file instead of reading a token others could read.
		IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
	return bSaved;
}
}
