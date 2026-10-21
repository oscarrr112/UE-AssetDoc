// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Utils/ClassFinderUtils.h"
#include "AssetDocumentServerModule.h"
#include "Engine/DataAsset.h"
#include "Components/Widget.h"
#include "Misc/App.h"
#include "UObject/UObjectIterator.h"

UClass* FClassFinderUtils::FindClassByName(
	const FString& ClassName,
	UClass* BaseClass,
	bool bSupportBlueprint)
{
	if (ClassName.IsEmpty())
	{
		return nullptr;
	}

	if (ClassName.StartsWith(TEXT("/")))
	{
		if (UClass* LoadedClass = FindObject<UClass>(nullptr, *ClassName))
		{
			return LoadedClass->IsChildOf(BaseClass) ? LoadedClass : nullptr;
		}

		if (UClass* LoadedClass = StaticLoadClass(BaseClass, nullptr, *ClassName, nullptr, LOAD_None, nullptr))
		{
			return LoadedClass;
		}

		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Could not find class '%s'"), *ClassName);
		return nullptr;
	}

	// Normalize: remove A/U prefix if present (UE class paths don't include prefix)
	FString SearchName = NormalizeClassName(ClassName, ClassName[0]);

	// Search in multiple modules
	TArray<FString> ModulePaths = GetDefaultModulePaths();

	for (const FString& ModulePath : ModulePaths)
	{
		UClass* FoundClass = TryLoadClassFromModule(ModulePath, SearchName, BaseClass);
		if (FoundClass)
		{
			return FoundClass;
		}
	}

	// Generic fallback: iterate all loaded classes
	if (UClass* FoundClass = FindClassByIterator(SearchName, BaseClass))
	{
		return FoundClass;
	}

	// Try loading Blueprint class if enabled
	if (bSupportBlueprint)
	{
		UClass* BlueprintClass = TryLoadBlueprintClass(ClassName, BaseClass);
		if (BlueprintClass)
		{
			return BlueprintClass;
		}
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("Could not find class '%s'"), *ClassName);
	return nullptr;
}

UClass* FClassFinderUtils::FindInterfaceClass(const FString& InterfaceName)
{
	if (InterfaceName.IsEmpty())
	{
		return nullptr;
	}

	// Remove leading 'I' if present for searching
	FString SearchName = NormalizeClassName(InterfaceName, TEXT('I'));

	// Interface classes are prefixed with 'U' in the registry
	TArray<FString> ModulePaths = GetDefaultModulePaths();

	for (const FString& ModulePath : ModulePaths)
	{
		FString ClassPath = FString::Printf(TEXT("%s.U%s"), *ModulePath, *SearchName);
		UClass* FoundClass = FindObject<UClass>(nullptr, *ClassPath);
		if (FoundClass && FoundClass->IsChildOf(UInterface::StaticClass()))
		{
			return FoundClass;
		}
	}

	// Generic fallback
	if (UClass* FoundClass = FindClassByIterator(SearchName, UInterface::StaticClass()))
	{
		return FoundClass;
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("Could not find interface class '%s'"), *InterfaceName);
	return nullptr;
}

UClass* FClassFinderUtils::FindWidgetClass(const FString& WidgetTypeName)
{
	if (WidgetTypeName.IsEmpty())
	{
		return nullptr;
	}

	// Try to load as full class path first
	if (WidgetTypeName.StartsWith(TEXT("/Script/")))
	{
		// Silent probe first
		UClass* LoadedClass = FindObject<UClass>(nullptr, *WidgetTypeName);
		if (LoadedClass && LoadedClass->IsChildOf(UWidget::StaticClass()))
		{
			return LoadedClass;
		}
		// Fallback to StaticLoadClass for lazy loading
		if (!LoadedClass)
		{
			LoadedClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *WidgetTypeName);
			if (LoadedClass)
			{
				return LoadedClass;
			}
		}
	}

	// Normalize the class name
	FString SearchName = NormalizeClassName(WidgetTypeName, TEXT('U'));

	// Try widget-specific module paths (UMG first)
	TArray<FString> ModulePaths = GetWidgetModulePaths();

	for (const FString& ModulePath : ModulePaths)
	{
		const TCHAR* Prefixes[] = { TEXT(""), TEXT("U") };
		for (const TCHAR* Prefix : Prefixes)
		{
			FString FullPath = FString::Printf(TEXT("%s.%s%s"), *ModulePath, Prefix, *SearchName);

			// Silent probe first
			UClass* FoundClass = FindObject<UClass>(nullptr, *FullPath);
			if (FoundClass && FoundClass->IsChildOf(UWidget::StaticClass()))
			{
				return FoundClass;
			}

			// StaticLoadClass fallback for lazy loading
			if (!FoundClass)
			{
				FoundClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *FullPath);
				if (FoundClass)
				{
					return FoundClass;
				}
			}
		}
	}

	// Try loading as Blueprint Widget
	UClass* BlueprintClass = TryLoadBlueprintClass(WidgetTypeName, UWidget::StaticClass());
	if (BlueprintClass)
	{
		return BlueprintClass;
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("Could not find widget class '%s'"), *WidgetTypeName);
	return nullptr;
}

UClass* FClassFinderUtils::FindDataAssetClass(const FString& ClassName)
{
	if (ClassName.IsEmpty())
	{
		return nullptr;
	}

	// Handle explicit request for base DataAsset
	if (ClassName.Equals(TEXT("DataAsset"), ESearchCase::IgnoreCase) ||
		ClassName.Equals(TEXT("UDataAsset"), ESearchCase::IgnoreCase))
	{
		return UDataAsset::StaticClass();
	}

	// Normalize: remove U prefix if present
	FString SearchName = NormalizeClassName(ClassName, TEXT('U'));

	// Search in multiple modules (use TryLoadClassFromModule which tries U/A prefixes)
	TArray<FString> ModulePaths = GetDefaultModulePaths();

	for (const FString& ModulePath : ModulePaths)
	{
		UClass* FoundClass = TryLoadClassFromModule(ModulePath, SearchName, UDataAsset::StaticClass());
		if (FoundClass)
		{
			return FoundClass;
		}
	}

	// Generic fallback
	if (UClass* FoundClass = FindClassByIterator(SearchName, UDataAsset::StaticClass()))
	{
		return FoundClass;
	}

	// Try loading Blueprint class
	UClass* BlueprintClass = TryLoadBlueprintClass(ClassName, UDataAsset::StaticClass());
	if (BlueprintClass)
	{
		return BlueprintClass;
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("Could not find DataAsset subclass '%s'"), *ClassName);
	return nullptr;
}

TArray<FString> FClassFinderUtils::GetDefaultModulePaths()
{
	return {
		TEXT("/Script/AssetDocumentServer"),                                     // This plugin
		FString::Printf(TEXT("/Script/%s"), FApp::GetProjectName()),      // Main project (dynamic)
		TEXT("/Script/Engine"),                                           // Engine
	};
}

TArray<FString> FClassFinderUtils::GetWidgetModulePaths()
{
	return {
		TEXT("/Script/UMG"),                                              // UMG (Widget priority)
		FString::Printf(TEXT("/Script/%s"), FApp::GetProjectName()),      // Main project (dynamic)
		TEXT("/Script/Engine"),                                           // Engine
		TEXT("/Script/AssetDocumentServer"),                                     // This plugin
	};
}

FString FClassFinderUtils::NormalizeClassName(const FString& ClassName, TCHAR ExpectedPrefix)
{
	if (ClassName.IsEmpty())
	{
		return ClassName;
	}

	// Check if starts with expected prefix (A, U, or I)
	if ((ExpectedPrefix == TEXT('A') || ExpectedPrefix == TEXT('U') || ExpectedPrefix == TEXT('I'))
		&& ClassName.Len() > 1
		&& ClassName[0] == ExpectedPrefix
		&& FChar::IsUpper(ClassName[1]))
	{
		return ClassName.Mid(1);
	}

	return ClassName;
}

UClass* FClassFinderUtils::TryLoadClassFromModule(
	const FString& ModulePath,
	const FString& ClassName,
	UClass* BaseClass)
{
	// For each prefix variant, try silent FindObject first, then StaticLoadClass as fallback
	const TCHAR* Prefixes[] = { TEXT(""), TEXT("U"), TEXT("A") };

	for (const TCHAR* Prefix : Prefixes)
	{
		FString ClassPath = FString::Printf(TEXT("%s.%s%s"), *ModulePath, Prefix, *ClassName);

		// 1. Silent probe - FindObject doesn't log warnings for missing classes
		UClass* FoundClass = FindObject<UClass>(nullptr, *ClassPath);
		if (FoundClass && FoundClass->IsChildOf(BaseClass))
		{
			return FoundClass;
		}

		// 2. StaticLoadClass fallback - needed to trigger lazy loading for classes not yet in memory
		if (!FoundClass)
		{
			FoundClass = StaticLoadClass(BaseClass, nullptr, *ClassPath, nullptr, LOAD_NoWarn, nullptr);
			if (FoundClass)
			{
				return FoundClass;
			}
		}
	}

	return nullptr;
}

UClass* FClassFinderUtils::TryLoadBlueprintClass(
	const FString& ClassName,
	UClass* BaseClass)
{
	// Normalize the class name for Blueprint lookup
	FString SearchName = ClassName;
	if (SearchName.Contains(TEXT("/")) || SearchName.Contains(TEXT(".")))
	{
		if (UClass* LoadedClass = LoadClass<UObject>(nullptr, *SearchName))
		{
			return LoadedClass->IsChildOf(BaseClass) ? LoadedClass : nullptr;
		}
		return nullptr;
	}

	if (SearchName.StartsWith(TEXT("A")) || SearchName.StartsWith(TEXT("U")))
	{
		if (SearchName.Len() > 1 && FChar::IsUpper(SearchName[1]))
		{
			SearchName = SearchName.Mid(1);
		}
	}

	// Try common Blueprint paths
	TArray<FString> BlueprintPaths = {
		FString::Printf(TEXT("/Game/Blueprints/%s.%s_C"), *SearchName, *SearchName),
		FString::Printf(TEXT("/Game/%s.%s_C"), *SearchName, *SearchName),
		FString::Printf(TEXT("/Game/UI/%s.%s_C"), *SearchName, *SearchName),
		FString::Printf(TEXT("/Game/Data/%s.%s_C"), *SearchName, *SearchName),
	};

	for (const FString& BlueprintPath : BlueprintPaths)
	{
		UClass* LoadedClass = LoadClass<UObject>(nullptr, *BlueprintPath);
		if (LoadedClass && LoadedClass->IsChildOf(BaseClass))
		{
			return LoadedClass;
		}
	}

	return nullptr;
}

UClass* FClassFinderUtils::FindClassByIterator(
	const FString& ClassName,
	UClass* BaseClass)
{
	// Cache: ClassName -> UClass* (avoids repeated iteration)
	static TMap<FString, TWeakObjectPtr<UClass>> Cache;

	FString CacheKey = ClassName + TEXT(":") + BaseClass->GetName();
	if (auto* Cached = Cache.Find(CacheKey))
	{
		if (Cached->IsValid())
		{
			return Cached->Get();
		}
		Cache.Remove(CacheKey);
	}

	// Try both with and without U/A prefix
	TArray<FString> NamesToMatch = { ClassName };
	if (!ClassName.StartsWith(TEXT("U")) && !ClassName.StartsWith(TEXT("A")))
	{
		NamesToMatch.Add(TEXT("U") + ClassName);
		NamesToMatch.Add(TEXT("A") + ClassName);
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class->IsChildOf(BaseClass))
		{
			continue;
		}

		FString Name = Class->GetName();
		for (const FString& Match : NamesToMatch)
		{
			if (Name == Match)
			{
				Cache.Add(CacheKey, Class);
				return Class;
			}
		}
	}

	return nullptr;
}
