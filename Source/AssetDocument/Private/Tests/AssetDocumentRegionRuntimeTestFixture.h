// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentJsonRegionUtils.h"
#include "AssetDocumentRegionRuntime.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

class FAssetDocumentRegionRuntimeTestContextBuilder
{
public:
	FAssetDocumentRegionRuntimeTestContextBuilder& WithRegionId(const FName InRegionId)
	{
		RegionId = InRegionId;
		return *this;
	}

	FAssetDocumentRegionRuntimeTestContextBuilder& WithBodyPath(const FString& InBodyPath)
	{
		BodyPath = InBodyPath;
		return *this;
	}

	FAssetDocumentRegionRuntimeTestContextBuilder& WithJsonPointer(const FString& InJsonPointer)
	{
		JsonPointer = InJsonPointer;
		return *this;
	}

	FAssetDocumentRegionRuntimeTestContextBuilder& WithPolicy(const FAssetDocumentRegionPolicy* InPolicy)
	{
		Policy = InPolicy;
		return *this;
	}

	FAssetDocumentRegionContext Build() const
	{
		FAssetDocumentRegionContext Context;
		Context.RegionId = RegionId;
		Context.BodyPath = BodyPath.IsEmpty()
			? FString::Printf(TEXT("Body.%s"), *RegionId.ToString())
			: BodyPath;
		Context.JsonPointer = JsonPointer;
		Context.Policy = Policy;
		return Context;
	}

private:
	FName RegionId = TEXT("Preview");
	FString BodyPath;
	FString JsonPointer = TEXT("/Body/Preview");
	const FAssetDocumentRegionPolicy* Policy = nullptr;
};

class FAssetDocumentRegionRuntimeTestFixture final
{
public:
	FAssetDocumentRegionRuntimeTestFixture() = delete;

	static TSharedPtr<FJsonValue> MakeObjectValue(TSharedRef<FJsonObject> Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	static TSharedPtr<FJsonValue> MakeArrayValue(TArray<TSharedPtr<FJsonValue>> Values)
	{
		return MakeShared<FJsonValueArray>(MoveTemp(Values));
	}

	static TSharedRef<FJsonValue> MakeObjectRef(TSharedRef<FJsonObject> Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	static TSharedRef<FJsonObject> MakeBodyWithField(const FString& FieldName, const TSharedPtr<FJsonValue>& FieldValue)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetField(FieldName, FieldValue);
		return Body;
	}

	static FAssetDocumentRegionPolicy MakePolicy(const FName RegionId, const FString& InBodyPath)
	{
		FAssetDocumentRegionPolicy Policy;
		Policy.RegionId = RegionId;
		Policy.BodyPath = InBodyPath;
		return Policy;
	}

	static FAssetDocumentRegionPolicy MakeDeferredPolicy(
		const FName RegionId,
		const FString& InBodyPath,
		const EAssetDocumentRegionKind RegionKind)
	{
		FAssetDocumentRegionPolicy Policy = MakePolicy(RegionId, InBodyPath);
		Policy.RegionKind = RegionKind;
		return Policy;
	}

	static FAssetDocumentRegionBinding MakeBinding(
		const FName BodyKey,
		const FName RegionId,
		const FName AdapterName,
		const int32 ApplyOrder = 0,
		const bool bRequired = false)
	{
		FAssetDocumentRegionBinding Binding;
		Binding.BodyKey = BodyKey;
		Binding.RegionId = RegionId;
		Binding.AdapterName = AdapterName;
		Binding.ApplyOrder = ApplyOrder;
		Binding.bRequired = bRequired;
		return Binding;
	}

	static FAssetDocumentRegionContext MakeRuntimeContext(
		const FName RegionId = TEXT("Preview"),
		const FString& JsonPointer = TEXT("/Body/Preview"),
		const FAssetDocumentRegionPolicy* Policy = nullptr)
	{
		return FAssetDocumentRegionRuntimeTestContextBuilder()
			.WithRegionId(RegionId)
			.WithJsonPointer(JsonPointer)
			.WithPolicy(Policy)
			.Build();
	}


	static FString GetDiffEntryPath(const TArray<TSharedPtr<FJsonValue>>& Entries, const int32 Index)
	{
		const TSharedPtr<FJsonObject> Entry = Entries.IsValidIndex(Index) && Entries[Index].IsValid()
			? Entries[Index]->AsObject()
			: nullptr;
		return Entry.IsValid() ? Entry->GetStringField(TEXT("path")) : FString();
	}

	static TSharedPtr<FJsonObject> FindDiffEntryByPath(
		const TArray<TSharedPtr<FJsonValue>>& Entries,
		const FString& ExpectedPath)
	{
		for (const TSharedPtr<FJsonValue>& EntryValue : Entries)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			FString Path;
			if (Entry.IsValid() && Entry->TryGetStringField(TEXT("path"), Path) && Path == ExpectedPath)
			{
				return Entry;
			}
		}
		return nullptr;
	}

	static bool TestDiagnostic(
		FAutomationTestBase* Test,
		const TCHAR* Label,
		const FAssetDocumentCapabilityResult& Result,
		const FString& ExpectedPath,
		const FString& ExpectedCode)
	{
		const FString ActualPath = Result.Diagnostics.Num() > 0 ? Result.Diagnostics[0].Path : FString();
		const FString ActualCode = Result.Diagnostics.Num() > 0 ? Result.Diagnostics[0].Code : FString();
		bool bPassed = true;
		bPassed &= Test->TestEqual(FString::Printf(TEXT("%s diagnostic path"), Label), ActualPath, ExpectedPath);
		bPassed &= Test->TestEqual(FString::Printf(TEXT("%s diagnostic code"), Label), ActualCode, ExpectedCode);
		return bPassed;
	}
};

#endif
