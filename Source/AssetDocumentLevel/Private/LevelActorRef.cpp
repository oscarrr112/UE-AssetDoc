// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentLevelActorRef.h"

#include "AssetDocumentNameSuggestions.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelWorldIndex.h"
#include "Misc/EditorPathHelper.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocumentLevel
{
using namespace UE::AssetDocumentLevel::Private;

FString GetActorPath(const AActor& Actor)
{
	return FEditorPathHelper::GetEditorPath(&Actor).ToString();
}

FString FormatActorGuid(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::Digits);
}

bool ParseActorRef(const TSharedPtr<FJsonValue>& Json, FActorRef& OutRef, FString& OutError)
{
	OutRef = FActorRef();
	if (!Json.IsValid())
	{
		OutError = TEXT("expected an actor reference: a path string, \"@id\", or {\"path\"} / {\"guid\"} / {\"label\", \"level\"?} / {\"id\"}.");
		return false;
	}
	FString Text;
	if (Json->TryGetString(Text))
	{
		if (Text.IsEmpty())
		{
			OutError = TEXT("an actor reference string must not be empty.");
			return false;
		}
		if (Text.StartsWith(TEXT("@")))
		{
			OutRef.Kind = EActorRefKind::BatchId;
			OutRef.Value = Text.RightChop(1);
			if (OutRef.Value.IsEmpty())
			{
				OutError = TEXT("\"@\" must be followed by a batch id.");
				return false;
			}
			return true;
		}
		OutRef.Kind = EActorRefKind::Path;
		OutRef.Value = Text;
		return true;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Json->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
	{
		OutError = TEXT("expected an actor reference: a path string, \"@id\", or {\"path\"} / {\"guid\"} / {\"label\", \"level\"?} / {\"id\"}.");
		return false;
	}
	const FJsonObject& Fields = **Object;
	static const TCHAR* const Kinds[] = {TEXT("path"), TEXT("guid"), TEXT("label"), TEXT("id")};
	int32 KindCount = 0;
	for (const TCHAR* Kind : Kinds)
	{
		KindCount += Fields.HasField(Kind) ? 1 : 0;
	}
	for (const auto& Field : Fields.Values)
	{
		if (Field.Key != TEXT("path") && Field.Key != TEXT("guid") && Field.Key != TEXT("label") && Field.Key != TEXT("id")
			&& Field.Key != TEXT("level"))
		{
			OutError = FString::Printf(TEXT("unknown actor reference field '%s' (use one of path, guid, label (+ level), id)."), *Field.Key);
			return false;
		}
	}
	if (KindCount != 1)
	{
		OutError = TEXT("an actor reference object names exactly one of \"path\", \"guid\", \"label\" or \"id\".");
		return false;
	}
	if (Fields.HasField(TEXT("level")) && !Fields.HasField(TEXT("label")))
	{
		OutError = TEXT("\"level\" only narrows a \"label\" lookup.");
		return false;
	}
	FString Value;
	if (Fields.TryGetStringField(TEXT("path"), Value))
	{
		OutRef.Kind = EActorRefKind::Path;
	}
	else if (Fields.TryGetStringField(TEXT("guid"), Value))
	{
		OutRef.Kind = EActorRefKind::Guid;
		FGuid Parsed;
		if (!FGuid::Parse(Value, Parsed) || !Parsed.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is not an actor guid (32 hex digits, as query_level returns it)."), *Value);
			return false;
		}
	}
	else if (Fields.TryGetStringField(TEXT("label"), Value))
	{
		OutRef.Kind = EActorRefKind::Label;
		if (Fields.HasField(TEXT("level")) && !Fields.TryGetStringField(TEXT("level"), OutRef.Level))
		{
			OutError = TEXT("\"level\" must be a level package path string (e.g. \"/Game/Maps/Room\").");
			return false;
		}
	}
	else if (Fields.TryGetStringField(TEXT("id"), Value))
	{
		OutRef.Kind = EActorRefKind::BatchId;
	}
	else
	{
		OutError = TEXT("actor reference fields are strings.");
		return false;
	}
	if (Value.IsEmpty())
	{
		OutError = TEXT("an actor reference value must not be empty.");
		return false;
	}
	OutRef.Value = Value;
	return true;
}

namespace
{
FActorRefResolution Resolved(AActor& Actor)
{
	FActorRefResolution Result;
	Result.bResolved = true;
	Result.Actor = &Actor;
	Result.Path = GetActorPath(Actor);
	Result.Guid = Actor.GetActorGuid();
	Result.bLoaded = true;
	return Result;
}

FActorRefResolution ResolvedUnloaded(const FUnloadedActor& Unloaded)
{
	FActorRefResolution Result;
	Result.bResolved = true;
	Result.Path = Unloaded.Path;
	Result.Guid = Unloaded.Guid;
	Result.bLoaded = false;
	return Result;
}

FActorRefResolution Failed(const TCHAR* Code, FString Message)
{
	FActorRefResolution Result;
	Result.Code = Code;
	Result.Message = MoveTemp(Message);
	return Result;
}

// A not-found reference names the closest actors: by label for a label reference (" Close labels: a, b."), by
// object name or label for a path (" Close: <path>, <path>."); Candidates holds their paths (details.candidates).
FActorRefResolution NotFound(UWorld& World, FString Message, const FString& Wanted, bool bPathRef)
{
	FActorRefResolution Result = Failed(TEXT("Level.ActorNotFound"), MoveTemp(Message));
	TArray<FString> Names;
	TArray<FString> Paths;
	auto Add = [&Names, &Paths, bPathRef](const FString& Name, const FString& Label, const FString& Path)
	{
		if (bPathRef)
		{
			Names.Add(Name);
			Paths.Add(Path);
		}
		Names.Add(Label);
		Paths.Add(Path);
	};
	ForEachListedActor(World, [&Add](AActor& Actor) { Add(Actor.GetName(), Actor.GetActorLabel(false), GetActorPath(Actor)); });
	TArray<FUnloadedActor> Storage;
	for (const FUnloadedActor& Unloaded : GetUnloadedActors(World, Storage))
	{
		Add(Unloaded.Name, Unloaded.Label, Unloaded.Path);
	}
	// A path's last segment is the object name ("...:PersistentLevel.PointLight_2").
	int32 Separator = INDEX_NONE;
	const FString Key = bPathRef && (Wanted.FindLastChar(TEXT('.'), Separator) || Wanted.FindLastChar(TEXT(':'), Separator))
		? Wanted.RightChop(Separator + 1) : Wanted;
	TArray<FString> Shown;
	for (const FString& Close : UE::AssetDocument::SuggestCloseNames(Key, Names, 5))
	{
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Names[Index] == Close && Result.Candidates.Num() < 8 && !Result.Candidates.Contains(Paths[Index]))
			{
				Result.Candidates.Add(Paths[Index]);
				Shown.AddUnique(bPathRef ? Paths[Index] : Close);
			}
		}
	}
	if (!Shown.IsEmpty())
	{
		Result.Message += FString::Printf(TEXT(" Close %s: %s."), bPathRef ? TEXT("actors") : TEXT("labels"), *FString::Join(Shown, TEXT(", ")));
	}
	return Result;
}

bool IsActorInWorld(const AActor& Actor, const UWorld& World)
{
	return IsListedActor(Actor) && Actor.GetWorld() == &World && Actor.GetLevel() != nullptr
		&& GetLoadedLevels(const_cast<UWorld&>(World)).Contains(Actor.GetLevel());
}
}

namespace LevelActorRefLocal
{
// A reference string that is not a path ("/..."): an Outliner label, then an object name ("KeyLight", "PointLight_0"),
// as query_level and query_play take them. Labels are case-sensitive (as {"label"}), object names are not. A label or
// a name shared by several actors is Level.ActorRefAmbiguous with their paths.
FActorRefResolution ResolveBareName(UWorld& World, const FActorRef& Ref)
{
	FActorRef ByLabel;
	ByLabel.Kind = EActorRefKind::Label;
	ByLabel.Value = Ref.Value;
	FActorRefResolution Label = ResolveActorRef(World, ByLabel);
	if (Label.bResolved || Label.Code == TEXT("Level.ActorRefAmbiguous"))
	{
		return Label;
	}
	TArray<FActorRefResolution> ByName;
	ForEachListedActor(World, [&ByName, &Ref](AActor& Actor)
	{
		if (Actor.GetName().Equals(Ref.Value, ESearchCase::IgnoreCase))
		{
			ByName.Add(Resolved(Actor));
		}
	});
	TArray<FUnloadedActor> Storage;
	for (const FUnloadedActor& Unloaded : GetUnloadedActors(World, Storage))
	{
		if (Unloaded.Name.Equals(Ref.Value, ESearchCase::IgnoreCase))
		{
			ByName.Add(ResolvedUnloaded(Unloaded));
		}
	}
	if (ByName.Num() == 1)
	{
		return ByName[0];
	}
	if (ByName.Num() > 1)
	{
		FActorRefResolution Ambiguous = Failed(TEXT("Level.ActorRefAmbiguous"), FString::Printf(
			TEXT("%d actors are named '%s' (in different levels). Use one of the candidate paths."), ByName.Num(), *Ref.Value));
		for (const FActorRefResolution& Match : ByName)
		{
			Ambiguous.Candidates.Add(Match.Path);
		}
		Ambiguous.Candidates.Sort();
		return Ambiguous;
	}
	Label.Message = FString::Printf(TEXT("No actor labelled or named '%s' in the editor world (an actor reference is a path as "
		"query_level returns it, an Outliner label or an object name; labels are case-sensitive).%s"), *Ref.Value,
		Label.Message.Contains(TEXT(" Close")) ? *Label.Message.RightChop(Label.Message.Find(TEXT(" Close"))) : TEXT(""));
	return Label;
}
} // namespace LevelActorRefLocal

FActorRefResolution ResolveActorRef(UWorld& World, const FActorRef& Ref, const TFunction<AActor*(FStringView Id)>& ResolveBatchId)
{
	switch (Ref.Kind)
	{
	case EActorRefKind::BatchId:
	{
		if (!ResolveBatchId)
		{
			return Failed(TEXT("Level.InvalidRequest"), FString::Printf(
				TEXT("'@%s' is a batch id; batch ids exist only inside one edit_level request. Use the actor's path."), *Ref.Value));
		}
		AActor* Actor = ResolveBatchId(Ref.Value);
		return Actor != nullptr ? Resolved(*Actor) : Failed(TEXT("Level.UnknownId"), FString::Printf(
			TEXT("'@%s' names no actor created earlier in this request."), *Ref.Value));
	}
	case EActorRefKind::Path:
	{
		if (!Ref.Value.StartsWith(TEXT("/")))
		{
			using namespace LevelActorRefLocal;
			return ResolveBareName(World, Ref);
		}
		const FSoftObjectPath Path(Ref.Value);
		if (AActor* Actor = Path.IsValid() ? Cast<AActor>(Path.ResolveObject()) : nullptr)
		{
			if (IsActorInWorld(*Actor, World))
			{
				return Resolved(*Actor);
			}
		}
		TArray<FUnloadedActor> Storage;
		for (const FUnloadedActor& Unloaded : GetUnloadedActors(World, Storage))
		{
			if (Unloaded.Path == Ref.Value)
			{
				return ResolvedUnloaded(Unloaded);
			}
		}
		return NotFound(World, FString::Printf(
			TEXT("No actor '%s' in the editor world (paths are \"<map>.<map>:PersistentLevel.<ObjectName>\" as query_level "
				"returns them; the object name is not the Outliner label)."), *Ref.Value), Ref.Value, true);
	}
	case EActorRefKind::Guid:
	{
		FGuid Guid;
		FGuid::Parse(Ref.Value, Guid);
		AActor* Found = nullptr;
		ForEachListedActor(World, [&](AActor& Actor)
		{
			if (Found == nullptr && Actor.GetActorGuid() == Guid)
			{
				Found = &Actor;
			}
		});
		if (Found != nullptr)
		{
			return Resolved(*Found);
		}
		TArray<FUnloadedActor> Storage;
		for (const FUnloadedActor& Unloaded : GetUnloadedActors(World, Storage))
		{
			if (Unloaded.Guid == Guid)
			{
				return ResolvedUnloaded(Unloaded);
			}
		}
		return Failed(TEXT("Level.ActorNotFound"), FString::Printf(TEXT("No actor with guid %s in the editor world."), *Ref.Value));
	}
	case EActorRefKind::Label:
	default:
	{
		TArray<FActorRefResolution> Matches;
		ForEachListedActor(World, [&](AActor& Actor)
		{
			if (Actor.GetActorLabel(false).Equals(Ref.Value, ESearchCase::CaseSensitive)
				&& (Ref.Level.IsEmpty() || GetLevelPackageName(*Actor.GetLevel()) == Ref.Level))
			{
				Matches.Add(Resolved(Actor));
			}
		});
		TArray<FUnloadedActor> Storage;
		for (const FUnloadedActor& Unloaded : GetUnloadedActors(World, Storage))
		{
			if (Unloaded.Label.Equals(Ref.Value, ESearchCase::CaseSensitive)
				&& (Ref.Level.IsEmpty() || Unloaded.LevelPackage == Ref.Level))
			{
				Matches.Add(ResolvedUnloaded(Unloaded));
			}
		}
		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		if (Matches.IsEmpty())
		{
			return NotFound(World, FString::Printf(
				TEXT("No actor labelled '%s'%s in the editor world (labels are case-sensitive)."), *Ref.Value,
				Ref.Level.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" in level %s"), *Ref.Level)), Ref.Value, false);
		}
		FActorRefResolution Ambiguous = Failed(TEXT("Level.ActorRefAmbiguous"), FString::Printf(
			TEXT("%d actors are labelled '%s'; labels are not unique. Use one of the candidate paths."), Matches.Num(), *Ref.Value));
		for (const FActorRefResolution& Match : Matches)
		{
			Ambiguous.Candidates.Add(Match.Path);
		}
		Ambiguous.Candidates.Sort();
		return Ambiguous;
	}
	}
}

TSharedRef<FJsonObject> MakeActorRefJson(const AActor& Actor)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("path"), GetActorPath(Actor));
	Json->SetStringField(TEXT("guid"), FormatActorGuid(Actor.GetActorGuid()));
	Json->SetStringField(TEXT("label"), Actor.GetActorLabel(false));
	Json->SetStringField(TEXT("class"), Actor.GetClass()->GetPathName());
	const ULevel* Level = Actor.GetLevel();
	Json->SetStringField(TEXT("level"), Level != nullptr ? GetLevelPackageName(*Level) : FString());
	if (const UPackage* External = Actor.GetExternalPackage())
	{
		Json->SetStringField(TEXT("external_package"), External->GetName());
	}
	else
	{
		Json->SetField(TEXT("external_package"), MakeShared<FJsonValueNull>());
	}
	Json->SetBoolField(TEXT("loaded"), true);
	return Json;
}
}
