// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentDuplicateFixups.h"

#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectThreadContext.h"

namespace
{
// Class paths, never UClass pointers: unregistering at module shutdown must not touch a purged class.
TMap<FTopLevelAssetPath, FAssetDocumentDuplicateFixup>& GetDuplicateFixups()
{
	static TMap<FTopLevelAssetPath, FAssetDocumentDuplicateFixup> Fixups;
	return Fixups;
}

FAssetDocumentDuplicateFixup FindDuplicateFixup(const UClass* Class)
{
	for (const UClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
	{
		if (const FAssetDocumentDuplicateFixup* Found = GetDuplicateFixups().Find(Current->GetClassPathName()))
		{
			return *Found;
		}
	}
	return nullptr;
}
} // namespace

bool RegisterAssetDocumentDuplicateFixup(const FTopLevelAssetPath& ClassPath, FAssetDocumentDuplicateFixup Fixup)
{
	check(IsInGameThread());
	if (ClassPath.IsNull() || Fixup == nullptr || GetDuplicateFixups().Contains(ClassPath))
	{
		return false;
	}
	GetDuplicateFixups().Add(ClassPath, Fixup);
	return true;
}

bool UnregisterAssetDocumentDuplicateFixup(const FTopLevelAssetPath& ClassPath)
{
	check(IsInGameThread());
	return GetDuplicateFixups().Remove(ClassPath) > 0;
}

bool HasAssetDocumentDuplicateFixup(const UClass& Class)
{
	return FindDuplicateFixup(&Class) != nullptr;
}

UObject* DuplicateObjectWithAssetDocumentFixups(FObjectDuplicationParameters& Parameters)
{
	const UClass* Class = Parameters.DestClass != nullptr ? Parameters.DestClass
		: Parameters.SourceObject != nullptr ? Parameters.SourceObject->GetClass() : nullptr;
	const FAssetDocumentDuplicateFixup Fixup = FindDuplicateFixup(Class);
	if (Fixup == nullptr || Parameters.bSkipPostLoad || !IsInGameThread())
	{
		return StaticDuplicateObjectEx(Parameters);
	}
	TMap<UObject*, UObject*> Created;
	TMap<UObject*, UObject*>* const CallerCreated = Parameters.CreatedObjects;
	Parameters.bSkipPostLoad = true;
	Parameters.CreatedObjects = &Created;
	UObject* Copy = StaticDuplicateObjectEx(Parameters);
	Parameters.bSkipPostLoad = false;
	Parameters.CreatedObjects = CallerCreated;
	if (CallerCreated != nullptr)
	{
		CallerCreated->Append(Created);
	}
	if (Copy == nullptr)
	{
		return nullptr;
	}
	Fixup(*Copy, Created);
	// As StaticDuplicateObjectEx post-loads its copies when not asked to skip it.
	for (const TPair<UObject*, UObject*>& Pair : Created)
	{
		if (UObject* Duplicated = Pair.Value; Duplicated != nullptr && !Duplicated->IsTemplate())
		{
			TGuardValue<bool> GuardIsRoutingPostLoad(FUObjectThreadContext::Get().IsRoutingPostLoad, true);
			Duplicated->ConditionalPostLoad();
		}
	}
	return Copy;
}
