// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Play/PlayLog.h"

#include "HAL/PlatformTime.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

namespace UE::AssetDocumentLevel::Private::Play
{
FPlayLog& FPlayLog::Get()
{
	static FPlayLog Instance;
	return Instance;
}

void FPlayLog::Register()
{
	if (!bRegistered && GLog != nullptr)
	{
		GLog->AddOutputDevice(this);
		bRegistered = true;
	}
}

void FPlayLog::Unregister()
{
	if (bRegistered && GLog != nullptr)
	{
		GLog->RemoveOutputDevice(this);
	}
	bRegistered = false;
}

uint64 FPlayLog::GetLastSeq() const
{
	FScopeLock ScopeLock(&Lock);
	return LastSeq;
}

uint64 FPlayLog::ReserveSeq()
{
	FScopeLock ScopeLock(&Lock);
	return ++LastSeq;
}

void FPlayLog::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	Serialize(Message, Verbosity, Category, -1.0);
}

void FPlayLog::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category, double Time)
{
	const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
	if (Message == nullptr || Level == ELogVerbosity::SetColor || Level == ELogVerbosity::NoLogging)
	{
		return;
	}
	FPlayLogEntry Entry;
	Entry.Time = FPlatformTime::Seconds();
	Entry.Category = Category;
	Entry.Verbosity = Level;
	Entry.Message = FString(Message).Left(MaxMessageChars);
	Entry.Message.TrimEndInline();

	FScopeLock ScopeLock(&Lock);
	Entry.Seq = ++LastSeq;
	if (Ring.Num() < Capacity)
	{
		Ring.Add(MoveTemp(Entry));
	}
	else
	{
		Ring[Head] = MoveTemp(Entry);
		Head = (Head + 1) % Capacity;
	}
}

namespace
{
bool MatchesLogFilter(const FPlayLogEntry& Entry, const FPlayLogFilter& Filter)
{
	if (Entry.Seq <= Filter.Since || Entry.Verbosity > Filter.MaxVerbosity)
	{
		return false;
	}
	if (!Filter.Categories.IsEmpty())
	{
		const FString Category = Entry.Category.ToString();
		if (!Filter.Categories.ContainsByPredicate([&Category](const FString& Wanted) { return Wanted.Equals(Category, ESearchCase::IgnoreCase); }))
		{
			return false;
		}
	}
	return Filter.Text.IsEmpty() || Entry.Message.Contains(Filter.Text, ESearchCase::IgnoreCase);
}
}

TArray<FPlayLogEntry> FPlayLog::Query(const FPlayLogFilter& Filter, int32 Limit, int32& OutSkipped, uint64& OutOldestKept) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<const FPlayLogEntry*> Matched;
	const int32 Count = Ring.Num();
	OutOldestKept = Count > 0 ? Ring[Count < Capacity ? 0 : Head].Seq : LastSeq + 1;
	for (int32 Offset = 0; Offset < Count; ++Offset)
	{
		const FPlayLogEntry& Entry = Ring[Count < Capacity ? Offset : (Head + Offset) % Capacity];
		if (MatchesLogFilter(Entry, Filter))
		{
			Matched.Add(&Entry);
		}
	}
	OutSkipped = FMath::Max(0, Matched.Num() - Limit);
	TArray<FPlayLogEntry> Result;
	for (int32 Index = OutSkipped; Index < Matched.Num(); ++Index)
	{
		Result.Add(*Matched[Index]);
	}
	return Result;
}

void FPlayLog::Summarize(uint64 Since, int32 MaxLines, int32& OutErrors, int32& OutWarnings, TArray<FPlayLogEntry>& OutLines) const
{
	FScopeLock ScopeLock(&Lock);
	OutErrors = 0;
	OutWarnings = 0;
	const int32 Count = Ring.Num();
	for (int32 Offset = 0; Offset < Count; ++Offset)
	{
		const FPlayLogEntry& Entry = Ring[Count < Capacity ? Offset : (Head + Offset) % Capacity];
		if (Entry.Seq <= Since || Entry.Verbosity > ELogVerbosity::Warning)
		{
			continue;
		}
		(Entry.Verbosity == ELogVerbosity::Warning ? OutWarnings : OutErrors) += 1;
		if (OutLines.Num() < MaxLines)
		{
			OutLines.Add(Entry);
		}
	}
}

bool ParseVerbosity(const FString& Text, ELogVerbosity::Type& Out)
{
	struct FName2Level
	{
		const TCHAR* Name;
		ELogVerbosity::Type Level;
	};
	static const FName2Level Levels[] = {
		{TEXT("error"), ELogVerbosity::Error},
		{TEXT("warning"), ELogVerbosity::Warning},
		{TEXT("display"), ELogVerbosity::Display},
		{TEXT("log"), ELogVerbosity::Log},
		{TEXT("verbose"), ELogVerbosity::VeryVerbose},
	};
	for (const FName2Level& Entry : Levels)
	{
		if (Text.Equals(Entry.Name, ESearchCase::IgnoreCase))
		{
			Out = Entry.Level;
			return true;
		}
	}
	return false;
}

FString VerbosityName(ELogVerbosity::Type Verbosity)
{
	switch (Verbosity)
	{
	case ELogVerbosity::Fatal: return TEXT("Fatal");
	case ELogVerbosity::Error: return TEXT("Error");
	case ELogVerbosity::Warning: return TEXT("Warning");
	case ELogVerbosity::Display: return TEXT("Display");
	case ELogVerbosity::Log: return TEXT("Log");
	case ELogVerbosity::Verbose: return TEXT("Verbose");
	default: return TEXT("VeryVerbose");
	}
}

TSharedRef<FJsonObject> LogEntryJson(const FPlayLogEntry& Entry, double BaseTime, int32 MaxChars)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("seq"), static_cast<double>(Entry.Seq));
	Json->SetNumberField(TEXT("t"), BaseTime > 0.0 ? FMath::RoundToDouble((Entry.Time - BaseTime) * 1000.0) / 1000.0 : 0.0);
	Json->SetStringField(TEXT("category"), Entry.Category.ToString());
	Json->SetStringField(TEXT("verbosity"), VerbosityName(Entry.Verbosity));
	Json->SetStringField(TEXT("message"), Entry.Message.Len() > MaxChars ? Entry.Message.Left(MaxChars) + TEXT("...") : Entry.Message);
	return Json;
}
}
