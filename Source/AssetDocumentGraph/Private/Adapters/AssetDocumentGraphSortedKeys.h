// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Algo/Sort.h"

namespace UE::AssetDocument::Graph
{
// Sorts Items by their key (FString ordering, as TArray::StableSort with operator< would) and keeps the first of the
// items whose keys are equal, in their original order: the result of a stable sort followed by removing adjacent
// duplicates. Every node spawn lists the whole Blueprint action database (~44,000 actions in the editor, ~1,400 of
// them registered twice), so this sorts indices and moves each item once: the in-place stable sort and a RemoveAt per
// duplicate (each moving the rest of the list) took ~1 s per spawn, minutes per spawn-heavy test.
template <typename ItemType, typename KeyOfType>
void SortUniqueByKey(TArray<ItemType>& Items, KeyOfType&& KeyOf)
{
	TArray<int32> Order;
	Order.Reserve(Items.Num());
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Algo::Sort(Order, [&Items, &KeyOf](int32 A, int32 B)
	{
		const FString& KeyA = KeyOf(Items[A]);
		const FString& KeyB = KeyOf(Items[B]);
		if (KeyA < KeyB)
		{
			return true;
		}
		return !(KeyB < KeyA) && A < B;
	});
	TArray<ItemType> Sorted;
	Sorted.Reserve(Items.Num());
	for (const int32 Index : Order)
	{
		if (Sorted.IsEmpty() || !(KeyOf(Sorted.Last()) == KeyOf(Items[Index])))
		{
			Sorted.Add(MoveTemp(Items[Index]));
		}
	}
	Items = MoveTemp(Sorted);
}
}
