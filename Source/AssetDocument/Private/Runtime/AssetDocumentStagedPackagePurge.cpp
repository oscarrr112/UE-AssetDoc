// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Runtime/AssetDocumentStagedPackagePurge.h"

#include "AssetDocumentModule.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocument::StagedPackages
{
namespace
{
int32 RequestDepth = 0;
TSet<FName> PendingPackages;
FTSTicker::FDelegateHandle DeferredPurge;

void SchedulePurgeOutsideRequests()
{
	if (DeferredPurge.IsValid())
	{
		return;
	}
	DeferredPurge = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		if (RequestDepth > 0)
		{
			return true; // the request's own scope purges
		}
		DeferredPurge.Reset();
		PurgeReleased();
		return false;
	}));
}
}

bool IsStagedCopy(const UPackage& Package)
{
	return Package.GetName().StartsWith(TEXT("/Temp/AssetDocument/Runtime/"), ESearchCase::CaseSensitive);
}

void NoteReleased(const UPackage& Package)
{
	check(IsInGameThread());
	PendingPackages.Add(Package.GetFName());
	if (RequestDepth == 0)
	{
		SchedulePurgeOutsideRequests();
	}
}

int32 PurgeReleased()
{
	check(IsInGameThread());
	if (PendingPackages.IsEmpty())
	{
		return 0;
	}
	const TSet<FName> Released = MoveTemp(PendingPackages);
	PendingPackages.Reset();
	const double Start = FPlatformTime::Seconds();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TArray<FString> Survivors;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (Released.Contains(It->GetFName()))
		{
			Survivors.Add(It->GetName());
		}
	}
	UE_LOG(LogAssetDocument, Verbose, TEXT("AssetDocument purged %d staged package(s) in %.3f s"),
		Released.Num() - Survivors.Num(), FPlatformTime::Seconds() - Start);
	if (!Survivors.IsEmpty())
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("AssetDocument: released staged copies are still referenced after garbage collection: %s"),
			*FString::Join(Survivors, TEXT(", ")));
	}
	return Survivors.Num();
}

FScopedRequest::FScopedRequest()
{
	check(IsInGameThread());
	++RequestDepth;
}

FScopedRequest::~FScopedRequest()
{
	if (--RequestDepth == 0)
	{
		PurgeReleased();
	}
}
}
