// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentTextDisplays.h"

namespace UE::AssetDocument::TextDisplays
{
namespace
{
// A read outside any primitive call (a C++ route, a test) also records; the bound keeps that from growing.
constexpr int32 MaximumRecords = 4096;

TMap<FString, FString>& GetAssetDocumentTextDisplayRecords()
{
	static TMap<FString, FString> Records;
	return Records;
}
}

void Record(const FString& Form, const FString& Display)
{
	if (!IsInGameThread() || Form.Equals(Display, ESearchCase::CaseSensitive))
	{
		return;
	}
	TMap<FString, FString>& Records = GetAssetDocumentTextDisplayRecords();
	if (Records.Num() < MaximumRecords || Records.Contains(Form))
	{
		Records.Add(Form, Display);
	}
}

void Reset()
{
	if (IsInGameThread())
	{
		GetAssetDocumentTextDisplayRecords().Reset();
	}
}

TMap<FString, FString> Take()
{
	if (!IsInGameThread())
	{
		return {};
	}
	TMap<FString, FString>& Records = GetAssetDocumentTextDisplayRecords();
	TMap<FString, FString> Taken = MoveTemp(Records);
	Records.Reset();
	return Taken;
}
}
