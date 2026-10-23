// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

// What a document change does beyond a value count (e.g. a stale DataTable document deleted a row
// while validate said only "would change 1 value(s)"):
//   - Removals: the entries an apply would remove (rows, keys, elements), named, also inside a region that diffs as one
//     Replace. Generic: every profile's diff entries {path, kind, before (asset), after (document)}.
//   - Base revision: the revision of an asset's default extract (BaseRevision in the HTTP extract), and which paths
//     changed since an earlier extract (remembered for a while, so a stale document can name them).
namespace UE::AssetDocument::DocumentChanges
{
struct FRemoval
{
	// JSON Pointer into the asset's extract (the "before" side).
	FString Path;
	// The entry's identity when it has one (its Name, Id or Key), else empty.
	FString Name;

	FString Describe() const;
};

// The entries the diff entries remove: a Remove entry, and inside a Replace the array elements (matched by Name, Id
// or Key, or by value; RenamedFrom counts as kept) and the keys of maps of entries (every value an object) that are
// only in the asset. Numbers and bools in a list are values, not entries: a changed coordinate (a node's Position) is
// not a removal; only as many of them as the list loses count. A field an object leaves out (Values, Properties) is reset, not removed, and is not counted, so a
// profile whose map of scalars is complete (StringTable Entries, a MetaSound input's PageDefaults) diffs it key by key:
// its removed keys are Remove entries, named by their key.
TArray<FRemoval> CollectRemovals(const TArray<TSharedPtr<FJsonValue>>& DiffEntries);

// ", removing N entr(ies): a, b, ... (+M more)", or empty without removals.
FString DescribeRemovals(const TArray<FRemoval>& Removals, int32 MaxNamed = 8);

// The table rows the entries change or add, by the "row" a DataTable / CurveTable diff entry names (two
// changed rows must not read as one value): " in rows A, B (adding C)", " adding rows C", or empty when no entry names a row.
// Removed rows are DescribeRemovals'. At most MaxNamed names per list, then "(+N more)".
FString DescribeChangedRows(const TArray<TSharedPtr<FJsonValue>>& DiffEntries, int32 MaxNamed = 10);

// Payload form: [{path, name?}].
TArray<TSharedPtr<FJsonValue>> RemovalsToJson(const TArray<FRemoval>& Removals);

// The revision of a default extract (16 hex digits of a hash of its JSON without BaseRevision).
FString ComputeRevision(const FJsonObject& DefaultExtract);

// The extract with BaseRevision set (right after Target).
TSharedPtr<FJsonObject> WithRevision(const FJsonObject& Extract, const FString& Revision);

// Remembers the default extract of a revision (a bounded number of recent ones, game thread).
void RememberRevision(const FString& Revision, const FString& Target, const TSharedPtr<FJsonObject>& DefaultExtract);

// The remembered extract of a revision and its Target, or null.
TSharedPtr<FJsonObject> RecallRevision(const FString& Revision, FString& OutTarget);

// An apply of a document based on Base left the asset at revision After. It does not make a later document with the
// same Base current (that may be another client's edit of the same extract): the Document.StaleBase message only says
// which revision such an apply returned. The latest per target and base, a bounded number.
void RememberApply(const FString& Target, const FString& Base, const FString& After);
bool WasAppliedFrom(const FString& Target, const FString& Base, const FString& Live);

// The JSON Pointers (into Live) at which Live differs from Base: changed values, added and removed keys and elements.
TArray<FString> CollectChangedPaths(const FJsonObject& Base, const FJsonObject& Live, int32 MaxPaths);
}
