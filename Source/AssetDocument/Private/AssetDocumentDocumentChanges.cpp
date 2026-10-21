// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentDocumentChanges.h"

#include "Algo/AllOf.h"
#include "AssetDocumentTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Hash/CityHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace AssetDocumentDocumentChangesLocal
{
using UE::AssetDocument::DocumentChanges::FRemoval;

const TCHAR* const IdentityKeys[] = {TEXT("Name"), TEXT("Id"), TEXT("Key")};

FString PointerToken(const FString& Token)
{
	return Token.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

FString Child(const FString& Path, const FString& Token)
{
	return (Path == TEXT("/") ? FString() : Path) + TEXT("/") + PointerToken(Token);
}

bool IsNull(const TSharedPtr<FJsonValue>& Value)
{
	return !Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::None;
}

FString IdentityOf(const TSharedPtr<FJsonValue>& Value, const TCHAR* Key)
{
	FString Identity;
	if (!IsNull(Value) && Value->Type == EJson::Object)
	{
		Value->AsObject()->TryGetStringField(Key, Identity);
	}
	return Identity;
}

FString AnyIdentity(const TSharedPtr<FJsonValue>& Value)
{
	for (const TCHAR* Key : IdentityKeys)
	{
		const FString Identity = IdentityOf(Value, Key);
		if (!Identity.IsEmpty())
		{
			return Identity;
		}
	}
	return FString();
}

// The key every element of both lists is identified by, or null.
const TCHAR* IdentityKey(const TArray<TSharedPtr<FJsonValue>>& Left, const TArray<TSharedPtr<FJsonValue>>& Right)
{
	for (const TCHAR* Key : IdentityKeys)
	{
		auto AllHave = [Key](const TArray<TSharedPtr<FJsonValue>>& Values)
		{
			return Algo::AllOf(Values, [Key](const TSharedPtr<FJsonValue>& Value) { return !IdentityOf(Value, Key).IsEmpty(); });
		};
		if ((!Left.IsEmpty() || !Right.IsEmpty()) && AllHave(Left) && AllHave(Right))
		{
			return Key;
		}
	}
	return nullptr;
}

bool Equal(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
{
	if (IsNull(Left) || IsNull(Right))
	{
		return IsNull(Left) == IsNull(Right);
	}
	return FJsonValue::CompareEqual(*Left, *Right);
}

FString ScalarName(const TSharedPtr<FJsonValue>& Value)
{
	FString Text;
	return !IsNull(Value) && Value->Type == EJson::String && Value->TryGetString(Text) ? Text : FString();
}

bool IsEntryMap(const FJsonObject& Object)
{
	return Algo::AllOf(Object.Values, [](const auto& Field)
		{ return !IsNull(Field.Value) && Field.Value->Type == EJson::Object; });
}

void AllRemoved(const TSharedPtr<FJsonValue>& Before, const FString& Path, TArray<FRemoval>& Out)
{
	if (IsNull(Before))
	{
		return;
	}
	if (Before->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Elements = Before->AsArray();
		for (int32 Index = 0; Index < Elements.Num(); ++Index)
		{
			Out.Add({Child(Path, LexToString(Index)), AnyIdentity(Elements[Index]).IsEmpty() ? ScalarName(Elements[Index]) : AnyIdentity(Elements[Index])});
		}
	}
	else if (Before->Type == EJson::Object && IsEntryMap(*Before->AsObject()))
	{
		for (const auto& Field : Before->AsObject()->Values)
		{
			Out.Add({Child(Path, FString(*Field.Key)), FString(*Field.Key)});
		}
	}
}

void Removed(const TSharedPtr<FJsonValue>& Before, const TSharedPtr<FJsonValue>& After, const FString& Path, TArray<FRemoval>& Out)
{
	if (IsNull(Before))
	{
		return;
	}
	if (IsNull(After) || After->Type != Before->Type)
	{
		if (IsNull(After))
		{
			AllRemoved(Before, Path, Out);
		}
		return;
	}
	if (Before->Type == EJson::Object)
	{
		// A key the document leaves out is a removal only in a map of entries (every value an object, as named
		// graphs); elsewhere (Values, Properties) an omitted field is reset to its default, not removed.
		const TSharedPtr<FJsonObject> AfterObject = After->AsObject();
		const bool bEntryMap = IsEntryMap(*Before->AsObject()) && IsEntryMap(*AfterObject);
		for (const auto& Field : Before->AsObject()->Values)
		{
			if (const TSharedPtr<FJsonValue>* Kept = AfterObject->Values.Find(Field.Key))
			{
				Removed(Field.Value, *Kept, Child(Path, FString(*Field.Key)), Out);
			}
			else if (bEntryMap)
			{
				Out.Add({Child(Path, FString(*Field.Key)), FString(*Field.Key)});
			}
		}
		return;
	}
	if (Before->Type != EJson::Array)
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>& Left = Before->AsArray();
	const TArray<TSharedPtr<FJsonValue>>& Right = After->AsArray();
	if (const TCHAR* Key = IdentityKey(Left, Right))
	{
		TMap<FString, TSharedPtr<FJsonValue>> ById;
		TSet<FString> RenamedFrom;
		for (const TSharedPtr<FJsonValue>& Element : Right)
		{
			ById.Add(IdentityOf(Element, Key), Element);
			FString Old;
			if (Element->AsObject()->TryGetStringField(TEXT("RenamedFrom"), Old))
			{
				RenamedFrom.Add(Old);
			}
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			const FString Identity = IdentityOf(Left[Index], Key);
			if (const TSharedPtr<FJsonValue>* Kept = ById.Find(Identity))
			{
				Removed(Left[Index], *Kept, Child(Path, LexToString(Index)), Out);
			}
			else if (!RenamedFrom.Contains(Identity))
			{
				Out.Add({Child(Path, LexToString(Index)), Identity});
			}
		}
		return;
	}
	const bool bScalars = Algo::AllOf(Left, [](const TSharedPtr<FJsonValue>& Value) { return IsNull(Value) || (Value->Type != EJson::Object && Value->Type != EJson::Array); });
	if (bScalars)
	{
		// A string the document drops (a tag, a name, an asset path) is a removed element. A number or bool is a value:
		// a changed coordinate of a Position or a component of a color is replaced, not removed, so only as many of
		// them as the list loses count (a moved node must not be reported as "removing .../Position/0").
		TArray<bool> Used;
		Used.SetNumZeroed(Right.Num());
		TArray<int32> UnmatchedValues;
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			int32 Match = INDEX_NONE;
			for (int32 Candidate = 0; Candidate < Right.Num() && Match == INDEX_NONE; ++Candidate)
			{
				if (!Used[Candidate] && Equal(Left[Index], Right[Candidate]))
				{
					Match = Candidate;
				}
			}
			if (Match != INDEX_NONE)
			{
				Used[Match] = true;
			}
			else if (!IsNull(Left[Index]) && Left[Index]->Type == EJson::String)
			{
				Out.Add({Child(Path, LexToString(Index)), ScalarName(Left[Index])});
			}
			else
			{
				UnmatchedValues.Add(Index);
			}
		}
		const int32 Lost = FMath::Max(0, Left.Num() - Right.Num());
		for (int32 Unmatched = 0; Unmatched < UnmatchedValues.Num() && Unmatched < Lost; ++Unmatched)
		{
			Out.Add({Child(Path, LexToString(UnmatchedValues[Unmatched])), FString()});
		}
		return;
	}
	for (int32 Index = 0; Index < Left.Num(); ++Index)
	{
		if (Right.IsValidIndex(Index))
		{
			Removed(Left[Index], Right[Index], Child(Path, LexToString(Index)), Out);
		}
		else
		{
			Out.Add({Child(Path, LexToString(Index)), AnyIdentity(Left[Index])});
		}
	}
}

void Changed(const TSharedPtr<FJsonValue>& Base, const TSharedPtr<FJsonValue>& Live, const FString& BasePath, const FString& LivePath,
	TArray<FString>& Out, int32 Max)
{
	if (Out.Num() >= Max || Equal(Base, Live))
	{
		return;
	}
	if (IsNull(Base) || IsNull(Live) || Base->Type != Live->Type || (Base->Type != EJson::Object && Base->Type != EJson::Array))
	{
		Out.Add(LivePath);
		return;
	}
	if (Base->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> BaseObject = Base->AsObject();
		const TSharedPtr<FJsonObject> LiveObject = Live->AsObject();
		for (const auto& Field : LiveObject->Values)
		{
			const TSharedPtr<FJsonValue>* Before = BaseObject->Values.Find(Field.Key);
			if (Before == nullptr)
			{
				Out.Add(Child(LivePath, FString(*Field.Key)) + TEXT(" (added)"));
			}
			else
			{
				Changed(*Before, Field.Value, Child(BasePath, FString(*Field.Key)), Child(LivePath, FString(*Field.Key)), Out, Max);
			}
			if (Out.Num() >= Max)
			{
				return;
			}
		}
		for (const auto& Field : BaseObject->Values)
		{
			if (!LiveObject->HasField(Field.Key) && Out.Num() < Max)
			{
				Out.Add(Child(BasePath, FString(*Field.Key)) + TEXT(" (removed)"));
			}
		}
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>& Left = Base->AsArray();
	const TArray<TSharedPtr<FJsonValue>>& Right = Live->AsArray();
	if (const TCHAR* Key = IdentityKey(Left, Right))
	{
		TMap<FString, int32> BaseIndex;
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			BaseIndex.Add(IdentityOf(Left[Index], Key), Index);
		}
		TSet<FString> Seen;
		for (int32 Index = 0; Index < Right.Num() && Out.Num() < Max; ++Index)
		{
			const FString Identity = IdentityOf(Right[Index], Key);
			Seen.Add(Identity);
			if (const int32* Before = BaseIndex.Find(Identity))
			{
				Changed(Left[*Before], Right[Index], Child(BasePath, LexToString(*Before)), Child(LivePath, LexToString(Index)), Out, Max);
			}
			else
			{
				Out.Add(FString::Printf(TEXT("%s (%s, added)"), *Child(LivePath, LexToString(Index)), *Identity));
			}
		}
		for (int32 Index = 0; Index < Left.Num() && Out.Num() < Max; ++Index)
		{
			const FString Identity = IdentityOf(Left[Index], Key);
			if (!Seen.Contains(Identity))
			{
				Out.Add(FString::Printf(TEXT("%s (%s, removed)"), *Child(BasePath, LexToString(Index)), *Identity));
			}
		}
		return;
	}
	for (int32 Index = 0; Index < FMath::Max(Left.Num(), Right.Num()) && Out.Num() < Max; ++Index)
	{
		if (!Left.IsValidIndex(Index))
		{
			Out.Add(Child(LivePath, LexToString(Index)) + TEXT(" (added)"));
		}
		else if (!Right.IsValidIndex(Index))
		{
			Out.Add(Child(BasePath, LexToString(Index)) + TEXT(" (removed)"));
		}
		else
		{
			Changed(Left[Index], Right[Index], Child(BasePath, LexToString(Index)), Child(LivePath, LexToString(Index)), Out, Max);
		}
	}
}

struct FRememberedRevision
{
	FString Revision;
	FString Target;
	TSharedPtr<FJsonObject> Extract;
};

TArray<FRememberedRevision>& RememberedRevisions()
{
	static TArray<FRememberedRevision> Revisions;
	return Revisions;
}

constexpr int32 MaxRememberedRevisions = 32;

struct FRememberedApply
{
	FString Target;
	FString Base;
	FString After;
};

TArray<FRememberedApply>& RememberedApplies()
{
	static TArray<FRememberedApply> Applies;
	return Applies;
}
} // namespace AssetDocumentDocumentChangesLocal

namespace UE::AssetDocument::DocumentChanges
{
FString FRemoval::Describe() const
{
	return Name.IsEmpty() ? Path : FString::Printf(TEXT("%s (%s)"), *Path, *Name);
}

TArray<FRemoval> CollectRemovals(const TArray<TSharedPtr<FJsonValue>>& DiffEntries)
{
	using namespace AssetDocumentDocumentChangesLocal;
	TArray<FRemoval> Removals;
	for (const TSharedPtr<FJsonValue>& EntryValue : DiffEntries)
	{
		const TSharedPtr<FJsonObject> Entry = !IsNull(EntryValue) && EntryValue->Type == EJson::Object ? EntryValue->AsObject() : nullptr;
		if (!Entry.IsValid())
		{
			continue;
		}
		FString Path;
		FString Kind;
		Entry->TryGetStringField(TEXT("path"), Path);
		Entry->TryGetStringField(TEXT("kind"), Kind);
		const TSharedPtr<FJsonValue> Before = Entry->TryGetField(TEXT("before"));
		if (Kind == TEXT("Remove"))
		{
			// A key of a map (StringTable Entries, an input's PageDefaults) is named by its key, an element by its
			// identity or scalar value.
			FString Name = AnyIdentity(Before);
			FString Token;
			if (Name.IsEmpty() && Path.Split(TEXT("/"), nullptr, &Token, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
				&& !Token.IsEmpty() && !Token.IsNumeric())
			{
				Name = Token.Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~"));
			}
			Removals.Add({Path, Name.IsEmpty() ? ScalarName(Before) : Name});
		}
		else if (Kind == TEXT("Replace"))
		{
			Removed(Before, Entry->TryGetField(TEXT("after")), Path, Removals);
		}
	}
	return Removals;
}

FString DescribeRemovals(const TArray<FRemoval>& Removals, int32 MaxNamed)
{
	if (Removals.IsEmpty())
	{
		return FString();
	}
	TArray<FString> Named;
	for (int32 Index = 0; Index < Removals.Num() && Index < MaxNamed; ++Index)
	{
		Named.Add(Removals[Index].Describe());
	}
	return FString::Printf(TEXT(", removing %d entr%s: %s%s"), Removals.Num(), Removals.Num() == 1 ? TEXT("y") : TEXT("ies"),
		*FString::Join(Named, TEXT(", ")),
		Removals.Num() > Named.Num() ? *FString::Printf(TEXT(" (+%d more)"), Removals.Num() - Named.Num()) : TEXT(""));
}

FString DescribeChangedRows(const TArray<TSharedPtr<FJsonValue>>& DiffEntries, int32 MaxNamed)
{
	TArray<FString> Changed;
	TArray<FString> Added;
	for (const TSharedPtr<FJsonValue>& EntryValue : DiffEntries)
	{
		const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() && EntryValue->Type == EJson::Object ? EntryValue->AsObject() : nullptr;
		FString Row;
		FString Kind;
		if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("row"), Row) || Row.IsEmpty())
		{
			continue;
		}
		Entry->TryGetStringField(TEXT("kind"), Kind);
		if (Kind == TEXT("Add"))
		{
			Added.AddUnique(Row);
		}
		else if (Kind != TEXT("Remove"))
		{
			Changed.AddUnique(Row);
		}
	}
	const auto Names = [MaxNamed](const TArray<FString>& Rows)
	{
		TArray<FString> Named(Rows.GetData(), FMath::Min(Rows.Num(), MaxNamed));
		return FString::Join(Named, TEXT(", ")) + (Rows.Num() > Named.Num() ? FString::Printf(TEXT(" (+%d more)"), Rows.Num() - Named.Num())
			: FString());
	};
	if (Changed.IsEmpty())
	{
		return Added.IsEmpty() ? FString() : FString::Printf(TEXT(" adding row%s %s"), Added.Num() == 1 ? TEXT("") : TEXT("s"), *Names(Added));
	}
	return FString::Printf(TEXT(" in row%s %s%s"), Changed.Num() == 1 ? TEXT("") : TEXT("s"), *Names(Changed),
		Added.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (adding %s)"), *Names(Added)));
}

TArray<TSharedPtr<FJsonValue>> RemovalsToJson(const TArray<FRemoval>& Removals)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FRemoval& Removal : Removals)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("path"), Removal.Path);
		if (!Removal.Name.IsEmpty())
		{
			Object->SetStringField(TEXT("name"), Removal.Name);
		}
		Values.Add(MakeShared<FJsonValueObject>(Object));
	}
	return Values;
}

FString ComputeRevision(const FJsonObject& DefaultExtract)
{
	const TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
	Copy->Values = DefaultExtract.Values;
	Copy->RemoveField(AssetDocumentBaseRevisionField);
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Copy, Writer);
	const FTCHARToUTF8 Utf8(*Text);
	return FString::Printf(TEXT("%016llx"), static_cast<unsigned long long>(CityHash64(Utf8.Get(), static_cast<uint32>(Utf8.Length()))));
}

TSharedPtr<FJsonObject> WithRevision(const FJsonObject& Extract, const FString& Revision)
{
	const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	bool bPlaced = false;
	for (const auto& Field : Extract.Values)
	{
		if (Field.Key == AssetDocumentBaseRevisionField)
		{
			continue;
		}
		Result->Values.Add(Field.Key, Field.Value);
		if (Field.Key == TEXT("Target"))
		{
			Result->SetStringField(AssetDocumentBaseRevisionField, Revision);
			bPlaced = true;
		}
	}
	if (!bPlaced)
	{
		Result->SetStringField(AssetDocumentBaseRevisionField, Revision);
	}
	return Result;
}

void RememberRevision(const FString& Revision, const FString& Target, const TSharedPtr<FJsonObject>& DefaultExtract)
{
	using namespace AssetDocumentDocumentChangesLocal;
	TArray<FRememberedRevision>& Revisions = RememberedRevisions();
	Revisions.RemoveAll([&Revision](const FRememberedRevision& Entry) { return Entry.Revision == Revision; });
	Revisions.Add({Revision, Target, DefaultExtract});
	if (Revisions.Num() > MaxRememberedRevisions)
	{
		Revisions.RemoveAt(0, Revisions.Num() - MaxRememberedRevisions);
	}
}

TSharedPtr<FJsonObject> RecallRevision(const FString& Revision, FString& OutTarget)
{
	using namespace AssetDocumentDocumentChangesLocal;
	for (const FRememberedRevision& Entry : RememberedRevisions())
	{
		if (Entry.Revision == Revision)
		{
			OutTarget = Entry.Target;
			return Entry.Extract;
		}
	}
	return nullptr;
}

void RememberApply(const FString& Target, const FString& Base, const FString& After)
{
	using namespace AssetDocumentDocumentChangesLocal;
	TArray<FRememberedApply>& Applies = RememberedApplies();
	Applies.RemoveAll([&](const FRememberedApply& Entry) { return Entry.Target == Target && Entry.Base == Base; });
	Applies.Add({Target, Base, After});
	if (Applies.Num() > MaxRememberedRevisions)
	{
		Applies.RemoveAt(0, Applies.Num() - MaxRememberedRevisions);
	}
}

bool WasAppliedFrom(const FString& Target, const FString& Base, const FString& Live)
{
	using namespace AssetDocumentDocumentChangesLocal;
	return RememberedApplies().ContainsByPredicate([&](const FRememberedApply& Entry)
		{ return Entry.Target == Target && Entry.Base == Base && Entry.After == Live; });
}

TArray<FString> CollectChangedPaths(const FJsonObject& Base, const FJsonObject& Live, int32 MaxPaths)
{
	using namespace AssetDocumentDocumentChangesLocal;
	const TSharedRef<FJsonObject> BaseCopy = MakeShared<FJsonObject>();
	BaseCopy->Values = Base.Values;
	BaseCopy->RemoveField(AssetDocumentBaseRevisionField);
	const TSharedRef<FJsonObject> LiveCopy = MakeShared<FJsonObject>();
	LiveCopy->Values = Live.Values;
	LiveCopy->RemoveField(AssetDocumentBaseRevisionField);
	TArray<FString> Paths;
	Changed(MakeShared<FJsonValueObject>(BaseCopy), MakeShared<FJsonValueObject>(LiveCopy), TEXT("/"), TEXT("/"), Paths, MaxPaths);
	return Paths;
}
}
