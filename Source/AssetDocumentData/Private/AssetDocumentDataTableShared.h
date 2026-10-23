// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Helpers shared by the DataTable and CurveTable primitives (composite parent tables, row limit).

#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Data
{
inline constexpr int32 DefaultRowLimit = 1000;

inline FArrayProperty* GetParentTablesProperty(UClass& Class)
{
	return FindFProperty<FArrayProperty>(&Class, TEXT("ParentTables"));
}

inline TArray<UObject*> ReadParentTables(UObject& Composite)
{
	TArray<UObject*> Parents;
	if (FArrayProperty* Property = GetParentTablesProperty(*Composite.GetClass()))
	{
		const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(Property->Inner);
		FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(&Composite));
		for (int32 Index = 0; Index < Helper.Num() && Inner != nullptr; ++Index)
		{
			Parents.Add(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)));
		}
	}
	return Parents;
}

// True when Candidate (a table or composite) reaches Self through its parent tables.
inline bool ReachesSelf(const UObject* Candidate, const UObject& Self, TSet<const UObject*>& Seen)
{
	if (Candidate == nullptr)
	{
		return false;
	}
	if (Candidate == &Self)
	{
		return true;
	}
	if (Seen.Contains(Candidate) || GetParentTablesProperty(*Candidate->GetClass()) == nullptr)
	{
		return false;
	}
	Seen.Add(Candidate);
	for (const UObject* Parent : ReadParentTables(*const_cast<UObject*>(Candidate)))
	{
		if (ReachesSelf(Parent, Self, Seen))
		{
			return true;
		}
	}
	return false;
}
} // namespace UE::AssetDocument::Graph::Data
