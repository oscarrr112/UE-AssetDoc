// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "AssetDocumentTypes.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentDiffCountTestSupport
{
// The entries of a Diff result: its "changed", "added" and "removed" lists together (as the Service counts them), or
// INDEX_NONE when the result failed or its payload has none of them (not a Diff result).
inline int32 CountDiffEntries(const FAssetDocumentResult& Result)
{
	int32 Count = INDEX_NONE;
	if (!Result.IsSuccess() || !Result.Payload.IsValid())
	{
		return Count;
	}
	for (const TCHAR* Bucket : {TEXT("changed"), TEXT("added"), TEXT("removed")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (Result.Payload->TryGetArrayField(Bucket, Entries))
		{
			Count = FMath::Max(Count, 0) + Entries->Num();
		}
	}
	return Count;
}
} // namespace AssetDocumentDiffCountTestSupport
#endif
