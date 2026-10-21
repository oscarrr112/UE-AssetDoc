// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentNameSuggestions.h"

namespace UE::AssetDocument
{
// Named (not anonymous): unity builds merge this file with others of the module (KI-8).
namespace NameSuggestionsPrivate
{
TArray<FString> SplitWords(const FString& Text)
{
	TArray<FString> Words;
	FString Current;
	auto Flush = [&Words, &Current]()
	{
		if (!Current.IsEmpty())
		{
			Words.Add(Current.ToLower());
			Current.Reset();
		}
	};
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Char = Text[Index];
		if (!FChar::IsAlnum(Char))
		{
			Flush();
			continue;
		}
		const bool bDigit = FChar::IsDigit(Char);
		if (!Current.IsEmpty())
		{
			const TCHAR Previous = Current[Current.Len() - 1];
			const bool bWasDigit = FChar::IsDigit(Previous);
			// A new word at a digit/letter change and at a lower-to-upper change ("CeilingLamp").
			if (bDigit != bWasDigit || (FChar::IsUpper(Char) && FChar::IsLower(Previous)))
			{
				Flush();
			}
		}
		Current.AppendChar(Char);
	}
	Flush();
	return Words;
}

// Edits from A to B; swapping two neighbours is one edit ("Spien" / "Spine").
int32 EditDistance(const FString& A, const FString& B)
{
	TArray<int32> Before;
	TArray<int32> Previous;
	TArray<int32> Current;
	Before.SetNumZeroed(B.Len() + 1);
	Previous.SetNumUninitialized(B.Len() + 1);
	Current.SetNumUninitialized(B.Len() + 1);
	for (int32 J = 0; J <= B.Len(); ++J)
	{
		Previous[J] = J;
	}
	for (int32 I = 1; I <= A.Len(); ++I)
	{
		Current[0] = I;
		for (int32 J = 1; J <= B.Len(); ++J)
		{
			const int32 Cost = A[I - 1] == B[J - 1] ? 0 : 1;
			Current[J] = FMath::Min3(Previous[J] + 1, Current[J - 1] + 1, Previous[J - 1] + Cost);
			if (I > 1 && J > 1 && A[I - 1] == B[J - 2] && A[I - 2] == B[J - 1])
			{
				Current[J] = FMath::Min(Current[J], Before[J - 2] + 1);
			}
		}
		Swap(Before, Previous);
		Swap(Previous, Current);
	}
	return Previous[B.Len()];
}

// Two words that name the same thing: equal, one the start of the other ("pluck" / "plucky") or one typo apart.
bool WordsMatch(const FString& Wanted, const FString& Word)
{
	if (Wanted == Word)
	{
		return true;
	}
	const int32 Shorter = FMath::Min(Wanted.Len(), Word.Len());
	if (Shorter >= 5 && (Wanted.StartsWith(Word) || Word.StartsWith(Wanted)))
	{
		return true;
	}
	return Shorter >= 5 && FMath::Abs(Wanted.Len() - Word.Len()) <= 1 && EditDistance(Wanted, Word) <= 1;
}

// Words most candidates share (a naming prefix such as "MSS" or "BP", "Bone" in "Bone(spine_01)"): sharing one says
// nothing about which candidate is meant, so it does not make a candidate close.
TSet<FString> CommonWords(const TArray<TArray<FString>>& WordsPerCandidate)
{
	TMap<FString, int32> Counts;
	for (const TArray<FString>& Words : WordsPerCandidate)
	{
		TSet<FString> Unique;
		Unique.Append(Words);
		for (const FString& Word : Unique)
		{
			++Counts.FindOrAdd(Word);
		}
	}
	TSet<FString> Common;
	for (const TPair<FString, int32>& Count : Counts)
	{
		if (Count.Value >= 3 && Count.Value * 4 >= WordsPerCandidate.Num())
		{
			Common.Add(Count.Key);
		}
	}
	return Common;
}

// 0 when not close; higher is closer.
int32 Closeness(const FString& WantedLower, const TArray<FString>& WantedWords, const FString& Candidate,
	const TArray<FString>& Words, const TSet<FString>& Common)
{
	const FString Lower = Candidate.ToLower();
	if (Lower.IsEmpty() || WantedLower.IsEmpty())
	{
		return 0;
	}
	if (Lower == WantedLower)
	{
		return 1000;
	}
	int32 Score = 0;
	if (FMath::Min(Lower.Len(), WantedLower.Len()) >= 3 && (Lower.Contains(WantedLower) || WantedLower.Contains(Lower)))
	{
		Score = FMath::Max(Score, 800 - FMath::Abs(Lower.Len() - WantedLower.Len()));
	}
	const int32 Distance = EditDistance(WantedLower, Lower);
	if (Distance <= FMath::Max(1, WantedLower.Len() / 4))
	{
		Score = FMath::Max(Score, 700 - 10 * Distance);
	}
	// Shared distinctive words: an equal word counts 2, a close one ("pluck" / "plucky") 1.
	int32 Shared = 0;
	for (const FString& Wanted : WantedWords)
	{
		if (Wanted.Len() < 3 || FChar::IsDigit(Wanted[0]) || Common.Contains(Wanted))
		{
			continue;
		}
		if (Words.Contains(Wanted))
		{
			Shared += 2;
		}
		else if (Words.ContainsByPredicate([&Wanted](const FString& Word) { return WordsMatch(Wanted, Word); }))
		{
			Shared += 1;
		}
	}
	if (Shared > 0)
	{
		Score = FMath::Max(Score, 400 + 25 * Shared - FMath::Min(Distance, 50));
	}
	return Score;
}
}

TArray<FString> SuggestCloseNames(const FString& Wanted, TConstArrayView<FString> Candidates, int32 Max)
{
	using namespace NameSuggestionsPrivate;
	const FString WantedLower = Wanted.ToLower();
	const TArray<FString> WantedWords = SplitWords(Wanted);
	TArray<const FString*> Unique;
	TArray<TArray<FString>> Words;
	TSet<FString> Seen;
	for (const FString& Candidate : Candidates)
	{
		if (!Seen.Contains(Candidate))
		{
			Seen.Add(Candidate);
			Unique.Add(&Candidate);
			Words.Add(SplitWords(Candidate));
		}
	}
	const TSet<FString> Common = CommonWords(Words);
	TArray<TPair<int32, const FString*>> Scored;
	for (int32 Index = 0; Index < Unique.Num(); ++Index)
	{
		const int32 Score = Closeness(WantedLower, WantedWords, *Unique[Index], Words[Index], Common);
		if (Score > 0)
		{
			Scored.Emplace(Score, Unique[Index]);
		}
	}
	Scored.Sort([](const TPair<int32, const FString*>& A, const TPair<int32, const FString*>& B)
	{
		if (A.Key != B.Key)
		{
			return A.Key > B.Key;
		}
		if (A.Value->Len() != B.Value->Len())
		{
			return A.Value->Len() < B.Value->Len();
		}
		return *A.Value < *B.Value;
	});
	TArray<FString> Result;
	for (int32 Index = 0; Index < Scored.Num() && Result.Num() < Max; ++Index)
	{
		Result.Add(*Scored[Index].Value);
	}
	return Result;
}

FString DescribeNameCandidates(const FString& Wanted, TConstArrayView<FString> Candidates, const TCHAR* Noun, int32 MaxListed)
{
	const TArray<FString> Close = SuggestCloseNames(Wanted, Candidates, 5);
	if (!Close.IsEmpty())
	{
		return FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", ")));
	}
	if (Candidates.IsEmpty())
	{
		return FString();
	}
	TArray<FString> All(Candidates.GetData(), Candidates.Num());
	All.Sort();
	const int32 Listed = FMath::Min(All.Num(), FMath::Max(MaxListed, 1));
	TArray<FString> Head(All.GetData(), Listed);
	FString Text = FString::Printf(TEXT(" %s: %s"), Noun, *FString::Join(Head, TEXT(", ")));
	if (All.Num() > Listed)
	{
		Text += FString::Printf(TEXT(" (+%d more)"), All.Num() - Listed);
	}
	return Text + TEXT(".");
}
}
