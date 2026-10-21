// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentAssetCompilation.h"

#include "AssetCompilingManager.h"
#include "Engine/Texture.h"
#include "Interfaces/Interface_AsyncCompilation.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace AssetDocumentAssetCompilation
{
void FinishPackageCompilation(TConstArrayView<UPackage*> Packages)
{
#if WITH_EDITOR
	check(IsInGameThread());
	TArray<UObject*> Compiling;
	TArray<UTexture*> Textures;
	for (UPackage* Package : Packages)
	{
		if (Package == nullptr)
		{
			continue;
		}
		ForEachObjectWithPackage(Package, [&Compiling, &Textures](UObject* Object)
		{
			if (const IInterface_AsyncCompilation* Async = Cast<IInterface_AsyncCompilation>(Object); Async != nullptr && Async->IsCompiling())
			{
				Compiling.Add(Object);
				if (UTexture* Texture = Cast<UTexture>(Object))
				{
					Textures.Add(Texture);
				}
			}
			return true;
		});
	}
	if (Compiling.IsEmpty())
	{
		return;
	}
	if (!Textures.IsEmpty())
	{
		FTextureCompilingManager::Get().FinishCompilation(Textures);
		for (UTexture* Texture : Textures)
		{
			Texture->BlockOnAnyAsyncBuild();
			if (Texture->IsCompiling())
			{
				Texture->UpdateResource();
			}
		}
	}
	FAssetCompilingManager::Get().FinishCompilationForObjects(Compiling);
#endif
}
}
