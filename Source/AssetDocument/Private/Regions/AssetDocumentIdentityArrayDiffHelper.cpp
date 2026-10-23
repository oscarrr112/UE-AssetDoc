// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Regions/AssetDocumentIdentityArrayDiffHelper.h"

#include "AssetDocumentJsonRegionUtils.h"

namespace
{
FString MakeDefaultPath(const FAssetDocumentIdentityArrayDiffOptions& Options, const FString& PathToken)
{
	return FString::Printf(
		TEXT("%s/%s"),
		*Options.RegionPath,
		*FAssetDocumentJsonRegionUtils::EscapeJsonPointerToken(PathToken));
}

FAssetDocumentCapabilityResult ValidateUniqueIdentities(
	const FAssetDocumentIdentityArrayDiffOptions& Options,
	const TArray<FAssetDocumentIdentityArrayDiffElement>& Elements,
	const TCHAR* Side)
{
	TSet<FString> Seen;
	for (const FAssetDocumentIdentityArrayDiffElement& Element : Elements)
	{
		if (Element.Identity.IsEmpty())
		{
			return FAssetDocumentJsonRegionUtils::Failure(
				Options.RegionPath,
				TEXT("MissingIdentityArrayDiffIdentity"),
				FString::Printf(TEXT("Identity array diff %s element is missing identity"), Side));
		}
		if (Seen.Contains(Element.Identity))
		{
			return FAssetDocumentJsonRegionUtils::Failure(
				Options.RegionPath,
				TEXT("DuplicateIdentityArrayDiffIdentity"),
				FString::Printf(TEXT("Duplicate identity array diff %s identity %s"), Side, *Element.Identity));
		}
		Seen.Add(Element.Identity);
	}
	return FAssetDocumentCapabilityResult::Success();
}

void AddIdentityDiffEntry(
	TArray<TSharedPtr<FJsonValue>>& Entries,
	const FString& Path,
	const FString& Status,
	const TSharedPtr<FJsonValue>& Current,
	const TSharedPtr<FJsonValue>& Desired,
	const FString& Change)
{
	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("path"), Path);
	Entry->SetStringField(TEXT("status"), Status);
	if (!Change.IsEmpty())
	{
		Entry->SetStringField(TEXT("change"), Change);
	}
	Entry->SetField(TEXT("current"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Current));
	Entry->SetField(TEXT("desired"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Desired));
	Entries.Add(MakeShared<FJsonValueObject>(Entry));
}

bool AreEntriesEqual(
	const FAssetDocumentIdentityArrayDiffEntryContext& Entry,
	const FAssetDocumentIdentityArrayDiffHooks& Hooks)
{
	if (!Entry.bHasCurrent || !Entry.bHasDesired)
	{
		return false;
	}
	if (Hooks.AreElementsEqual)
	{
		return Hooks.AreElementsEqual(Entry);
	}
	return FAssetDocumentJsonRegionUtils::JsonValueToComparableString(Entry.CurrentValue) ==
		FAssetDocumentJsonRegionUtils::JsonValueToComparableString(Entry.DesiredValue);
}

FString MakeChange(
	const FAssetDocumentIdentityArrayDiffEntryContext& Entry,
	const FAssetDocumentIdentityArrayDiffOptions& Options,
	const FAssetDocumentIdentityArrayDiffHooks& Hooks)
{
	if (!Entry.bHasDesired)
	{
		return Options.ExtraChange;
	}
	if (!Entry.bHasCurrent)
	{
		return Options.MissingChange;
	}
	return Hooks.MakeChange ? Hooks.MakeChange(Entry) : Options.ChangedChange;
}
}

FAssetDocumentCapabilityResult FAssetDocumentIdentityArrayDiffHelper::Diff(
	const FAssetDocumentIdentityArrayDiffOptions& Options,
	const TArray<FAssetDocumentIdentityArrayDiffElement>& CurrentElements,
	const TArray<FAssetDocumentIdentityArrayDiffElement>& DesiredElements,
	const FAssetDocumentIdentityArrayDiffHooks& Hooks,
	TArray<TSharedPtr<FJsonValue>>& OutDiffEntries)
{
	if (Options.RegionPath.IsEmpty())
	{
		return FAssetDocumentJsonRegionUtils::Failure(
			TEXT("/Body"),
			TEXT("MissingIdentityArrayDiffRegionPath"),
			TEXT("Identity array diff requires a region path"));
	}

	FAssetDocumentCapabilityResult Result = ValidateUniqueIdentities(Options, CurrentElements, TEXT("current"));
	if (!Result.bSuccess)
	{
		return Result;
	}
	Result = ValidateUniqueIdentities(Options, DesiredElements, TEXT("desired"));
	if (!Result.bSuccess)
	{
		return Result;
	}

	TMap<FString, const FAssetDocumentIdentityArrayDiffElement*> DesiredByIdentity;
	for (const FAssetDocumentIdentityArrayDiffElement& Desired : DesiredElements)
	{
		DesiredByIdentity.Add(Desired.Identity, &Desired);
	}

	TSet<FString> SeenDesired;
	const auto EmitEntry = [&](
		const FAssetDocumentIdentityArrayDiffElement* Current,
		const FAssetDocumentIdentityArrayDiffElement* Desired)
	{
		FAssetDocumentIdentityArrayDiffEntryContext Entry;
		Entry.Identity = Current ? Current->Identity : Desired->Identity;
		const FString PathToken = Current ? Current->PathToken : Desired->PathToken;
		Entry.CurrentValue = Current ? Current->Value : MakeShared<FJsonValueNull>();
		Entry.DesiredValue = Desired ? Desired->Value : MakeShared<FJsonValueNull>();
		Entry.bHasCurrent = Current != nullptr;
		Entry.bHasDesired = Desired != nullptr;
		Entry.Path = MakeDefaultPath(Options, PathToken);
		if (Hooks.MakePath)
		{
			Entry.Path = Hooks.MakePath(Entry);
		}

		const bool bEqual = AreEntriesEqual(Entry, Hooks);
		if (bEqual && !Options.bEmitUnchanged)
		{
			return;
		}

		const FString Status = bEqual ? TEXT("unchanged") : TEXT("changed");
		const FString Change = bEqual ? FString() : MakeChange(Entry, Options, Hooks);
		AddIdentityDiffEntry(
			OutDiffEntries,
			Entry.Path,
			Status,
			Entry.CurrentValue,
			Entry.DesiredValue,
			Change);
	};

	for (const FAssetDocumentIdentityArrayDiffElement& Current : CurrentElements)
	{
		const FAssetDocumentIdentityArrayDiffElement* const* Desired = DesiredByIdentity.Find(Current.Identity);
		if (Desired)
		{
			SeenDesired.Add(Current.Identity);
			EmitEntry(&Current, *Desired);
		}
		else
		{
			EmitEntry(&Current, nullptr);
		}
	}

	for (const FAssetDocumentIdentityArrayDiffElement& Desired : DesiredElements)
	{
		if (!SeenDesired.Contains(Desired.Identity))
		{
			EmitEntry(nullptr, &Desired);
		}
	}

	return FAssetDocumentCapabilityResult::Success(TEXT("Identity array diff completed"));
}
