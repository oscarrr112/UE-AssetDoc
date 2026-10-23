// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// data.string_table.read.v1 / data.string_table.edit.v1: namespace, entries and entry metadata
// of a UStringTable, as the String Table editor edits them (Modify, then FStringTable on
// GetMutableStringTable; metadata as the CSV import sets it).

#include "AssetDocumentDataPrimitives.h"

#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/TextNamespaceUtil.h"
#include "Misc/EngineVersionComparison.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "AssetDocumentStringTable"

namespace UE::AssetDocument::Graph::Data
{
namespace
{
constexpr int32 DefaultEntryLimit = 2000;

UStringTable* GetStringTable(FCall& Call)
{
	UStringTable* Table = Cast<UStringTable>(&Call.Asset);
	if (Table == nullptr)
	{
		Fail(Call, TEXT("Data.UnsupportedAsset"), TEXT("StringTable primitives edit StringTable assets."), TEXT("/HandleArguments/asset"));
	}
	return Table;
}

// FStringTableEditor::IsValidIdentity: keys and the namespace.
bool IsValidIdentity(const FString& Identity, FString& OutReason)
{
	const FString InvalidChars = FString::Printf(TEXT("%s%c%c"), INVALID_NAME_CHARACTERS,
		TextNamespaceUtil::PackageNamespaceStartMarker, TextNamespaceUtil::PackageNamespaceEndMarker);
	FText Reason;
	if (Identity.IsEmpty() || Identity.TrimStartAndEnd().IsEmpty())
	{
		OutReason = TEXT("It must not be empty.");
		return false;
	}
	if (!FName::IsValidXName(Identity, InvalidChars, &Reason))
	{
		OutReason = Reason.ToString();
		return false;
	}
	return true;
}

TArray<FString> SortedKeys(const FStringTable& Table)
{
	TArray<FString> Keys;
	Table.EnumerateSourceStrings([&Keys](const FString& Key, const FString&) { Keys.Add(Key); return true; });
	Keys.Sort();
	return Keys;
}
} // namespace

bool HandleStringTableRead(FCall& Call, FObjectBuilder& Result)
{
	UStringTable* Asset = GetStringTable(Call);
	int64 Offset = 0;
	int64 Limit = DefaultEntryLimit;
	if (Asset == nullptr || !Call.Args.OnlyFields({TEXT("offset"), TEXT("limit")})
		|| !Call.Args.Int(TEXT("offset"), Offset, false) || !Call.Args.Int(TEXT("limit"), Limit, false))
	{
		return false;
	}
	if (Offset < 0 || Limit < 1)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0 and limit >= 1."), TEXT("offset"));
	}
	const FStringTableConstRef Table = Asset->GetStringTable();
	Result.AddString(TEXT("namespace"), Table->GetNamespace());
	const TArray<FString> Keys = SortedKeys(*Table);
	TArray<FValue> Entries;
	int32 NodeCount = 8;
	const int32 NextOffset = AppendPaged(Entries, NodeCount, [&](int32 Index)
	{
		FString Source;
		Table->GetSourceString(FTextKey(Keys[Index]), Source);
		FObjectBuilder MetaData;
		TArray<TPair<FName, FString>> Pairs;
		Table->EnumerateMetaData(FTextKey(Keys[Index]), [&Pairs](FName Id, const FString& Value) { Pairs.Emplace(Id, Value); return true; });
		Pairs.Sort([](const TPair<FName, FString>& A, const TPair<FName, FString>& B) { return A.Key.LexicalLess(B.Key); });
		for (const TPair<FName, FString>& Pair : Pairs)
		{
			MetaData.AddString(Pair.Key.ToString(), Pair.Value);
		}
		FObjectBuilder Entry;
		Entry.AddString(TEXT("key"), Keys[Index]);
		Entry.AddString(TEXT("source"), Source);
		Entry.Add(TEXT("metadata"), MetaData.Build());
		return Entry.Build();
	}, static_cast<int32>(Offset), Keys.Num(), static_cast<int32>(Limit));
	Result.AddInt(TEXT("total"), Keys.Num());
	Result.AddInt(TEXT("next_offset"), NextOffset);
	Result.Add(TEXT("entries"), MakeArray(MoveTemp(Entries)));
	return true;
}

bool HandleStringTableEdit(FCall& Call, FObjectBuilder& Result)
{
	UStringTable* Asset = GetStringTable(Call);
	const FValue* Ops = nullptr;
	FString Namespace;
	if (Asset == nullptr || !Call.Args.OnlyFields({TEXT("namespace"), TEXT("ops")})
		|| !Call.Args.String(TEXT("namespace"), Namespace, false) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	// One editor transaction for the whole call (the per-edit transactions nest into it); the
	// table itself is not recorded (FTableUndoRecordingSuppression).
	const FTableUndoRecordingSuppression NoUndoRecording(*Asset);
	const FScopedTransaction CallTransaction(LOCTEXT("EditStringTable", "Edit String Table"));
	FString Reason;
	if (Call.Args.Has(TEXT("namespace")))
	{
		if (!IsValidIdentity(Namespace, Reason))
		{
			return Call.Args.Fail(TEXT("StringTable.InvalidNamespace"), FString::Printf(TEXT("'%s' is not a valid namespace: %s"), *Namespace, *Reason), TEXT("namespace"));
		}
		if (!Asset->GetStringTable()->GetNamespace().Equals(Namespace, ESearchCase::CaseSensitive))
		{
			const FScopedTransaction Transaction(LOCTEXT("SetNamespace", "Set Namespace"));
			Asset->Modify();
			Asset->GetMutableStringTable()->SetNamespace(Namespace);
		}
	}
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		FString Key;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Data.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !Op.String(TEXT("key"), Key))
		{
			return false;
		}
		if (!IsValidIdentity(Key, Reason))
		{
			return Op.Fail(TEXT("StringTable.InvalidKey"), FString::Printf(TEXT("'%s' is not a valid key: %s"), *Key, *Reason), TEXT("key"));
		}
		FString Existing;
		const bool bExists = Asset->GetStringTable()->GetSourceString(FTextKey(Key), Existing);
		if (Kind == TEXT("set"))
		{
			FString Source;
			if (!Op.OnlyFields({TEXT("op"), TEXT("key"), TEXT("source")}) || !Op.String(TEXT("source"), Source))
			{
				return false;
			}
			if (Source.IsEmpty())
			{
				return Op.Fail(TEXT("StringTable.EmptySourceString"), TEXT("The String Table editor does not add entries with an empty source string."), TEXT("source"));
			}
			if (!bExists || !Existing.Equals(Source, ESearchCase::CaseSensitive))
			{
				const FScopedTransaction Transaction(LOCTEXT("SetEntry", "Set String Table Entry"));
				Asset->Modify();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
				// Entries carry developer notes; the editor keeps an entry's notes when its source string changes.
				const FStringTableEntryConstPtr Entry = Asset->GetStringTable()->FindEntry(FTextKey(Key));
				Asset->GetMutableStringTable()->SetSourceString(FTextKey(Key), Source, Entry.IsValid() ? Entry->GetDevNotes() : FString());
#else
				Asset->GetMutableStringTable()->SetSourceString(FTextKey(Key), Source);
#endif
			}
			continue;
		}
		if (!bExists)
		{
			return Op.Fail(TEXT("StringTable.KeyNotFound"), FString::Printf(TEXT("The table has no key '%s'."), *Key), TEXT("key"));
		}
		if (Kind == TEXT("remove"))
		{
			if (!Op.OnlyFields({TEXT("op"), TEXT("key")}))
			{
				return false;
			}
			const FScopedTransaction Transaction(LOCTEXT("DeleteEntry", "Delete String Table Entry"));
			Asset->Modify();
			Asset->GetMutableStringTable()->RemoveSourceString(FTextKey(Key));
		}
		else if (Kind == TEXT("meta_set") || Kind == TEXT("meta_remove"))
		{
			FString Id;
			FString Value;
			const bool bSet = Kind == TEXT("meta_set");
			if (!(bSet ? Op.OnlyFields({TEXT("op"), TEXT("key"), TEXT("id"), TEXT("value")}) : Op.OnlyFields({TEXT("op"), TEXT("key"), TEXT("id")}))
				|| !Op.String(TEXT("id"), Id) || (bSet && !Op.String(TEXT("value"), Value)))
			{
				return false;
			}
			if (!IsValidRowName(Id, Reason))
			{
				return Op.Fail(TEXT("StringTable.InvalidMetaDataId"), FString::Printf(TEXT("'%s' is not a valid metadata id: %s"), *Id, *Reason), TEXT("id"));
			}
			const FString Current = Asset->GetStringTable()->GetMetaData(FTextKey(Key), FName(*Id));
			if (bSet ? Current.Equals(Value, ESearchCase::CaseSensitive) : Current.IsEmpty())
			{
				continue;
			}
			const FScopedTransaction Transaction(LOCTEXT("SetMetaData", "Set String Table Entry Metadata"));
			Asset->Modify();
			if (bSet)
			{
				Asset->GetMutableStringTable()->SetMetaData(FTextKey(Key), FName(*Id), Value);
			}
			else
			{
				Asset->GetMutableStringTable()->RemoveMetaData(FTextKey(Key), FName(*Id));
			}
		}
		else
		{
			return Op.Fail(TEXT("Data.InvalidOp"), FString::Printf(TEXT("Unknown op '%s' (set, remove, meta_set, meta_remove)."), *Kind), TEXT("op"));
		}
	}
	Result.AddInt(TEXT("entries"), SortedKeys(*Asset->GetStringTable()).Num());
	return true;
}
} // namespace UE::AssetDocument::Graph::Data

#undef LOCTEXT_NAMESPACE
