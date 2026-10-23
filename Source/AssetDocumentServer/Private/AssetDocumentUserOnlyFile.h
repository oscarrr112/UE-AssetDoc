// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

namespace AssetDocumentUserOnlyFile
{
/**
 * Creates or replaces Path with Text (UTF-8) so that only the current user can read or write it (the service
 * discovery file can hold the bearer token). The text always goes to a new file that is moved over Path, so a reader
 * that opened the old file never sees the new text. Mac and Linux: the new file is created (O_EXCL, mode 0600) in a
 * new folder next to Path that is first restricted to the current user (mode 0700), and renamed over Path; on the
 * Mac every extended ACL entry of the folder and the file (explicit, or inherited) is removed and checked to be gone
 * before anything is created or written (on Linux, the mode also masks POSIX ACL entries for other users and
 * groups). Windows: a protected DACL (no inherited entries) that allows only the current user and SYSTEM; the file
 * is written to a temporary file created with that DACL, checked, and moved over Path. When the restriction cannot
 * be applied, Path is deleted, nothing is written and OutError says why.
 */
bool Save(const FString& Text, const FString& Path, FString& OutError);
}
