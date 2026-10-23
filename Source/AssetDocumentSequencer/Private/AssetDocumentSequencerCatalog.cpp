// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Sequencer types (design §3.8) for list_types (kinds sequencer_track / sequencer_section / sequencer_binding,
// AssetDocumentTypeListers.h) and for seq.catalog.v1 ({kind: track | section | binding, name?, path?, base?, offset?,
// limit?} -> {total, next_offset, rows[]}, the same rows; track rows for the class of the sequence it is called on).
//   track:   the track classes a sequence class supports, as the "+ Track" menus offer them
//            (UMovieSceneSequence::IsTrackSupported on the class defaults, which applies UMovieScene::IsTrackClassAllowed):
//            {Path, Type (document token), Support: Supported | Default, Sections: [section class paths]}. Query
//            "schema" names the sequence class (default /Script/LevelSequence.LevelSequence).
//   section: the section classes: {Path, Properties: [writable fields], Channels?: [{Name, Type}]}. Channels come from a
//            transient section of the class (its channel proxy) for the engine's MovieScene / MovieSceneTracks classes,
//            and for any class named exactly by "path".
//   binding: the custom binding types (MovieSceneHelpers::GetPrioritySortedCustomBindingTypes, the binding menu's
//            "Convert to" list): {Path, Spawnable?, Replaceable?, Properties: [editable fields]}.

#include "AssetDocumentSequencerPrimitives.h"

#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneReplaceableBinding.h"
#include "Bindings/MovieSceneSpawnableBinding.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MovieSceneCommonHelpers.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
const TCHAR* const DefaultSequenceClass = TEXT("/Script/LevelSequence.LevelSequence");

FString QueryValue(const TMap<FString, FString>& Query, const TCHAR* Key)
{
	const FString* Value = Query.Find(Key);
	return Value != nullptr ? Value->TrimStartAndEnd() : FString();
}

bool Matches(const UClass& Class, const TMap<FString, FString>& Query, FString& OutError)
{
	const FString Path = Class.GetPathName();
	const FString Pattern = QueryValue(Query, TEXT("name"));
	if (!Pattern.IsEmpty())
	{
		const bool bWildcard = Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?"));
		auto Test = [&Pattern, bWildcard](const FString& Text)
		{
			return bWildcard ? Text.MatchesWildcard(Pattern, ESearchCase::IgnoreCase) : Text.Contains(Pattern, ESearchCase::IgnoreCase);
		};
		if (!Test(Path) && !Test(Class.GetName()))
		{
			return false;
		}
	}
	const FString Prefix = QueryValue(Query, TEXT("path"));
	if (!Prefix.IsEmpty() && !Path.StartsWith(Prefix, ESearchCase::IgnoreCase))
	{
		return false;
	}
	const FString Base = QueryValue(Query, TEXT("base"));
	if (!Base.IsEmpty())
	{
		const UClass* BaseClass = FindObject<UClass>(nullptr, *Base);
		if (BaseClass == nullptr)
		{
			OutError = FString::Printf(TEXT("Unknown base class '%s'."), *Base);
			return false;
		}
		return Class.IsChildOf(BaseClass);
	}
	return true;
}

TArray<TSharedPtr<FJsonValue>> FieldNames(const UClass& Class, TFunctionRef<bool(const FProperty&)> Include)
{
	TArray<TSharedPtr<FJsonValue>> Names;
	for (TFieldIterator<FProperty> It(&Class); It; ++It)
	{
		if (Include(**It))
		{
			Names.Add(MakeShared<FJsonValueString>(It->GetName()));
		}
	}
	return Names;
}

bool CollectRows(const FString& Kind, const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	if (Kind == TEXT("track"))
	{
		return ListTrackTypes(Query, OutRows, OutError);
	}
	if (Kind == TEXT("section"))
	{
		return ListSectionTypes(Query, OutRows, OutError);
	}
	if (Kind == TEXT("binding"))
	{
		return ListBindingTypes(Query, OutRows, OutError);
	}
	OutError = FString::Printf(TEXT("Unknown catalog kind '%s'. Kinds: track, section, binding."), *Kind);
	return false;
}
} // namespace

bool ListTrackTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	FString SchemaPath = QueryValue(Query, TEXT("schema"));
	SchemaPath = SchemaPath.IsEmpty() ? FString(DefaultSequenceClass) : SchemaPath;
	const UClass* SequenceClass = LoadObject<UClass>(nullptr, *SchemaPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	const UMovieSceneSequence* Sequence = SequenceClass != nullptr ? Cast<UMovieSceneSequence>(SequenceClass->GetDefaultObject()) : nullptr;
	if (Sequence == nullptr)
	{
		OutError = FString::Printf(TEXT("schema '%s' is not a sequence class (e.g. %s)."), *SchemaPath, DefaultSequenceClass);
		return false;
	}
	const TArray<UClass*> Sections = ConcreteSubclasses(*UMovieSceneSection::StaticClass());
	for (UClass* Class : ConcreteSubclasses(*UMovieSceneTrack::StaticClass()))
	{
		if (!Matches(*Class, Query, OutError))
		{
			if (!OutError.IsEmpty())
			{
				return false;
			}
			continue;
		}
		const ETrackSupport Support = Sequence->IsTrackSupported(Class);
		if (Support == ETrackSupport::NotSupported)
		{
			continue;
		}
		const UMovieSceneTrack* Defaults = Cast<UMovieSceneTrack>(Class->GetDefaultObject());
		TArray<TSharedPtr<FJsonValue>> SectionPaths;
		for (UClass* Section : Sections)
		{
			if (Defaults != nullptr && TakesSectionClass(*Defaults, Section))
			{
				SectionPaths.Add(MakeShared<FJsonValueString>(Section->GetPathName()));
			}
		}
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Class->GetPathName());
		Row->SetStringField(TEXT("Type"), TrackTypeToken(*Class));
		Row->SetStringField(TEXT("Support"), Support == ETrackSupport::Supported ? TEXT("Supported") : TEXT("Default"));
		Row->SetArrayField(TEXT("Sections"), SectionPaths);
		OutRows.Add(Row);
	}
	return true;
}

bool ListSectionTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	const FString Exact = QueryValue(Query, TEXT("path"));
	for (UClass* Class : ConcreteSubclasses(*UMovieSceneSection::StaticClass()))
	{
		if (!Matches(*Class, Query, OutError))
		{
			if (!OutError.IsEmpty())
			{
				return false;
			}
			continue;
		}
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Class->GetPathName());
		Row->SetArrayField(TEXT("Properties"), FieldNames(*Class, [](const FProperty& Property) { return IsSectionPropertyField(Property); }));
		const FName Package = Class->GetOutermost()->GetFName();
		if (Package == TEXT("/Script/MovieScene") || Package == TEXT("/Script/MovieSceneTracks") || Class->GetPathName() == Exact)
		{
			const UMovieSceneSection* Section = NewObject<UMovieSceneSection>(GetTransientPackage(), Class, NAME_None, RF_Transient);
			Row->SetArrayField(TEXT("Channels"), DescribeChannels(*Section));
		}
		TArray<TSharedPtr<FJsonValue>> Parameters = DescribeParameters(*Class);
		if (!Parameters.IsEmpty())
		{
			Row->SetArrayField(TEXT("Parameters"), Parameters);
		}
		OutRows.Add(Row);
	}
	return true;
}

bool ListBindingTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	TArray<const TSubclassOf<UMovieSceneCustomBinding>> Types;
	MovieSceneHelpers::GetPrioritySortedCustomBindingTypes(Types);
	for (const TSubclassOf<UMovieSceneCustomBinding>& Type : Types)
	{
		const UClass* Class = Type.Get();
		if (Class == nullptr || !Matches(*Class, Query, OutError))
		{
			if (!OutError.IsEmpty())
			{
				return false;
			}
			continue;
		}
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Class->GetPathName());
		if (Class->IsChildOf(UMovieSceneSpawnableBindingBase::StaticClass()))
		{
			Row->SetBoolField(TEXT("Spawnable"), true);
		}
		if (Class->IsChildOf(UMovieSceneReplaceableBindingBase::StaticClass()))
		{
			Row->SetBoolField(TEXT("Replaceable"), true);
		}
		Row->SetArrayField(TEXT("Properties"), FieldNames(*Class, [](const FProperty& Property)
		{
			return Property.HasAnyPropertyFlags(CPF_Edit) && IsStoredField(Property);
		}));
		OutRows.Add(Row);
	}
	return true;
}

bool HandleCatalog(FCall& Call, FObjectBuilder& Result)
{
	FString Kind;
	FString Name;
	FString Path;
	FString Base;
	int64 Offset = 0;
	int64 Limit = EnumerateDefaultLimit;
	if (!Call.Args.OnlyFields({TEXT("kind"), TEXT("name"), TEXT("path"), TEXT("base"), TEXT("offset"), TEXT("limit")})
		|| !Call.Args.String(TEXT("kind"), Kind) || !Call.Args.String(TEXT("name"), Name, false)
		|| !Call.Args.String(TEXT("path"), Path, false) || !Call.Args.String(TEXT("base"), Base, false)
		|| !Call.Args.Int(TEXT("offset"), Offset, false) || !Call.Args.Int(TEXT("limit"), Limit, false))
	{
		return false;
	}
	if (Offset < 0 || Limit < 1 || Limit > EnumerateMaximumLimit)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("offset must be >= 0 and limit 1..%d."), EnumerateMaximumLimit), TEXT("offset"));
	}
	TMap<FString, FString> Query;
	Query.Add(TEXT("name"), Name);
	Query.Add(TEXT("path"), Path);
	Query.Add(TEXT("base"), Base);
	Query.Add(TEXT("schema"), Call.Asset.GetClass()->GetPathName());
	TArray<TSharedPtr<FJsonObject>> Rows;
	FString Error;
	if (!CollectRows(Kind, Query, Rows, Error))
	{
		return Call.Args.Fail(TEXT("Sequencer.UnknownCatalogKind"), Error, TEXT("kind"));
	}
	Rows.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		return A->GetStringField(TEXT("Path")) < B->GetStringField(TEXT("Path"));
	});
	TArray<FValue> Page;
	int32 NodeCount = 8;
	int32 Index = static_cast<int32>(FMath::Min<int64>(Offset, Rows.Num()));
	for (; Index < Rows.Num() && Page.Num() < Limit; ++Index)
	{
		FValue Row = JsonToValue(MakeShared<FJsonValueObject>(Rows[Index]));
		const int32 RowNodes = CountValueNodes(Row);
		if (!Page.IsEmpty() && NodeCount + RowNodes > QueryValueNodeBudget)
		{
			break;
		}
		NodeCount += RowNodes;
		Page.Add(MoveTemp(Row));
	}
	Result.AddInt(TEXT("total"), Rows.Num());
	Result.AddInt(TEXT("next_offset"), Index < Rows.Num() ? Index : -1);
	Result.Add(TEXT("rows"), MakeArray(MoveTemp(Page)));
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer
