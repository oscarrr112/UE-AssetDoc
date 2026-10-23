// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// "Did you mean" for not-found diagnostics (a bone, an actor label, a component name): the
// candidates closest to what was written, so the message itself names the fix (an AI reads the
// message first; details may be skipped).
namespace UE::AssetDocument
{
// At most Max candidates close to Wanted, closest first: equal ignoring case, then one
// containing the other, then a small edit distance, then sharing a word ("CeilingLamp" ~
// "CeilingLight", "MSS_Plucky" ~ "MSS_Pluck_Dark"; words split at case changes, digits, '_', '-',
// '.', ' '; a word one starts the other or one typo apart counts half). A word most candidates
// share (a naming prefix like "MSS_" or "BP_") does not make a candidate close. Empty when
// nothing is close.
ASSETDOCUMENT_API TArray<FString> SuggestCloseNames(const FString& Wanted, TConstArrayView<FString> Candidates, int32 Max = 5);

// A sentence for a not-found message: " Close: a, b." when some candidates are close to Wanted,
// else " <Noun>: a, b, c (+N more)." listing up to MaxListed of all candidates (sorted), else ""
// when there are none.
ASSETDOCUMENT_API FString DescribeNameCandidates(const FString& Wanted, TConstArrayView<FString> Candidates, const TCHAR* Noun,
	int32 MaxListed = 12);
}
