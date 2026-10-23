// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDevice.h"

// The editor's output log as the playtest tools read it (query_play what: "log", the log summary of
// every play result): a bounded in-memory copy of every log line, numbered, so a caller can ask for
// the lines since the play session started or since a number an earlier result returned.
// Blueprint Print String (LogBlueprintUserMessages), Blueprint runtime errors ("Accessed None",
// runaway loops: LogScript), ensures and every other category arrive here like in the Output Log.
// Registered with GLog for the module's lifetime; any thread may log.
namespace UE::AssetDocumentLevel::Private::Play
{
struct FPlayLogEntry
{
	uint64 Seq = 0;
	double Time = 0.0;
	FName Category;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Message;
};

struct FPlayLogFilter
{
	// Entries with Seq > Since.
	uint64 Since = 0;
	// Exact category names (case-insensitive); empty: all.
	TArray<FString> Categories;
	// The most verbose level listed (Warning: warnings, errors and fatals).
	ELogVerbosity::Type MaxVerbosity = ELogVerbosity::Log;
	// Case-insensitive substring; empty: all.
	FString Text;
};

class FPlayLog final : public FOutputDevice
{
public:
	static constexpr int32 Capacity = 20000;
	static constexpr int32 MaxMessageChars = 2000;

	static FPlayLog& Get();

	void Register();
	void Unregister();

	// The newest sequence number (0: nothing logged yet).
	uint64 GetLastSeq() const;

	// A new sequence number for an event recorded beside the log (a sound started: query_play what "audio"), so
	// one "since" (an earlier result's last_seq) orders log lines and those events alike. No line is added.
	uint64 ReserveSeq();

	// Matches after Filter.Since, oldest first; at most Limit of the newest matches (OutSkipped: older
	// matches left out). OutOldestKept: the oldest Seq still in the buffer.
	TArray<FPlayLogEntry> Query(const FPlayLogFilter& Filter, int32 Limit, int32& OutSkipped, uint64& OutOldestKept) const;

	// Errors and warnings after Since: counts and up to MaxLines lines.
	void Summarize(uint64 Since, int32 MaxLines, int32& OutErrors, int32& OutWarnings, TArray<FPlayLogEntry>& OutLines) const;

	// FOutputDevice
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category, double Time) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

private:
	mutable FCriticalSection Lock;
	TArray<FPlayLogEntry> Ring;
	int32 Head = 0;
	uint64 LastSeq = 0;
	bool bRegistered = false;
};

// "error", "warning", "display", "log" (default), "verbose"; false for anything else.
bool ParseVerbosity(const FString& Text, ELogVerbosity::Type& Out);
FString VerbosityName(ELogVerbosity::Type Verbosity);

// {seq, t, category, verbosity, message}; t is seconds since BaseTime (the session start).
TSharedRef<FJsonObject> LogEntryJson(const FPlayLogEntry& Entry, double BaseTime, int32 MaxChars = FPlayLog::MaxMessageChars);
}
