// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"

/**
 * Utility class for finding UClass instances by name (AssetDocument's class resolver uses it).
 *
 * Features:
 * - Automatic handling of A/U prefixes for class names
 * - Support for Blueprint-generated classes
 * - Multi-module search (Engine, Project, Plugin)
 * - Interface class lookup (with I prefix handling)
 * - Widget class lookup (UMG priority)
 */
class ASSETDOCUMENTSERVER_API FClassFinderUtils
{
public:
	/**
	 * Find a class by name with optional base class constraint.
	 * Automatically handles A/U prefixes and searches multiple modules.
	 *
	 * @param ClassName - The class name to find (with or without A/U prefix)
	 * @param BaseClass - Base class constraint (defaults to UObject)
	 * @param bSupportBlueprint - Whether to also search for Blueprint classes
	 * @return The found class, or nullptr if not found
	 */
	static UClass* FindClassByName(
		const FString& ClassName,
		UClass* BaseClass = UObject::StaticClass(),
		bool bSupportBlueprint = true);

	/**
	 * Find an interface class by name.
	 * Handles I prefix normalization (e.g., "IMyInterface" -> searches for "UMyInterface").
	 *
	 * @param InterfaceName - The interface name (with or without I prefix)
	 * @return The found interface class, or nullptr if not found
	 */
	static UClass* FindInterfaceClass(const FString& InterfaceName);

	/**
	 * Find a widget class by name.
	 * Prioritizes UMG module and common widget types.
	 *
	 * @param WidgetTypeName - The widget type name (e.g., "TextBlock", "Button")
	 * @return The found widget class, or nullptr if not found
	 */
	static UClass* FindWidgetClass(const FString& WidgetTypeName);

	/**
	 * Find a DataAsset subclass by name.
	 *
	 * @param ClassName - The class name to find
	 * @return The found DataAsset class, or nullptr if not found
	 */
	static UClass* FindDataAssetClass(const FString& ClassName);

private:
	/**
	 * Get the default list of module paths to search.
	 * Includes: Current plugin, Project, Engine
	 */
	static TArray<FString> GetDefaultModulePaths();

	/**
	 * Get the module paths prioritized for widget classes.
	 * Includes: UMG, Project, Engine
	 */
	static TArray<FString> GetWidgetModulePaths();

	/**
	 * Normalize a class name by handling prefixes.
	 * @param ClassName - The class name to normalize
	 * @param ExpectedPrefix - The prefix to remove if present ('A', 'U', or 'I')
	 * @return The normalized class name without the prefix
	 */
	static FString NormalizeClassName(const FString& ClassName, TCHAR ExpectedPrefix);

	/**
	 * Try to load a class from a specific module path.
	 * @param ModulePath - The module path (e.g., "/Script/Engine")
	 * @param ClassName - The class name without prefix
	 * @param BaseClass - Base class constraint
	 * @return The found class, or nullptr if not found
	 */
	static UClass* TryLoadClassFromModule(
		const FString& ModulePath,
		const FString& ClassName,
		UClass* BaseClass);

	/**
	 * Try to load a Blueprint class from content path.
	 * @param ClassName - The class name
	 * @param BaseClass - Base class constraint
	 * @return The found Blueprint class, or nullptr if not found
	 */
	static UClass* TryLoadBlueprintClass(
		const FString& ClassName,
		UClass* BaseClass);

	/**
	 * Generic fallback: iterate all loaded UClass objects to find a match.
	 * Slower than module path lookup but works for any module.
	 * Results are cached for subsequent lookups.
	 * @param ClassName - The class name (without U/A prefix)
	 * @param BaseClass - Base class constraint
	 * @return The found class, or nullptr if not found
	 */
	static UClass* FindClassByIterator(
		const FString& ClassName,
		UClass* BaseClass);
};
