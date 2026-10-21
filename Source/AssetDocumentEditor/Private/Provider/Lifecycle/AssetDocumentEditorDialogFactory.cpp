// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorDialogFactory.h"

#include "UObject/Class.h"

namespace
{
TMap<FTopLevelAssetPath, FAssetDocumentEditorDialogFactoryInit>& DialogFactories()
{
	static TMap<FTopLevelAssetPath, FAssetDocumentEditorDialogFactoryInit> Registry;
	return Registry;
}
}

void RegisterAssetDocumentEditorDialogFactory(const FTopLevelAssetPath& FactoryClassPath, FAssetDocumentEditorDialogFactoryInit Init)
{
	check(IsInGameThread());
	if (FactoryClassPath.IsValid() && Init)
	{
		DialogFactories().Add(FactoryClassPath, MoveTemp(Init));
	}
}

void UnregisterAssetDocumentEditorDialogFactory(const FTopLevelAssetPath& FactoryClassPath)
{
	check(IsInGameThread());
	DialogFactories().Remove(FactoryClassPath);
}

const FAssetDocumentEditorDialogFactoryInit* FindAssetDocumentEditorDialogFactory(const UClass& FactoryClass)
{
	check(IsInGameThread());
	return DialogFactories().Find(FTopLevelAssetPath(&FactoryClass));
}
