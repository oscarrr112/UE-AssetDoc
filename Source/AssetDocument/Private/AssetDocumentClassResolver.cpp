// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentClassResolver.h"

#include "AssetDocumentExactBlueprintClasses.h"

#include "Utils/ClassFinderUtils.h"

#include "Animation/AnimBlueprint.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace
{
// Class paths, never UClass pointers or weak pointers: unregistering at module shutdown must not touch a purged class.
TArray<FTopLevelAssetPath>& GetExactBlueprintClasses()
{
	static TArray<FTopLevelAssetPath> Classes;
	return Classes;
}

bool IsExactBlueprintClass(const UClass* Class)
{
	return GetExactBlueprintClasses().Contains(Class->GetClassPathName());
}
} // namespace

bool RegisterAssetDocumentExactBlueprintClass(const FTopLevelAssetPath& BlueprintClassPath)
{
	check(IsInGameThread());
	if (BlueprintClassPath.IsNull() || GetExactBlueprintClasses().Contains(BlueprintClassPath))
	{
		return false;
	}
	GetExactBlueprintClasses().Add(BlueprintClassPath);
	return true;
}

bool UnregisterAssetDocumentExactBlueprintClass(const FTopLevelAssetPath& BlueprintClassPath)
{
	check(IsInGameThread());
	return GetExactBlueprintClasses().Remove(BlueprintClassPath) > 0;
}

bool IsAssetDocumentExactBlueprintClass(const UClass* BlueprintClass)
{
	return BlueprintClass != nullptr && IsExactBlueprintClass(BlueprintClass);
}

bool FAssetDocumentClassResolver::ResolveClassReference(const FString& ClassName, UClass*& OutClass, FString& OutError)
{
	OutClass = nullptr;

	FString NormalizedClassName = ClassName;
	NormalizedClassName.TrimStartAndEndInline();
	if (NormalizedClassName.IsEmpty())
	{
		OutError = TEXT("Class is required");
		return false;
	}

	if (!NormalizedClassName.StartsWith(TEXT("/")))
	{
		OutClass = FClassFinderUtils::FindClassByName(NormalizedClassName, UObject::StaticClass(), true);
	}

	if (!OutClass)
	{
		OutClass = StaticLoadClass(UObject::StaticClass(), nullptr, *NormalizedClassName);
	}

	if (!OutClass)
	{
		OutClass = FindObject<UClass>(nullptr, *NormalizedClassName);
	}

	if (!OutClass)
	{
		OutError = TEXT("Failed to resolve asset class");
		return false;
	}
	return true;
}

bool FAssetDocumentClassResolver::ResolveClass(const FString& ClassName, UClass*& OutClass, FString& OutError)
{
	FString Code;
	return ResolveClass(ClassName, OutClass, OutError, Code);
}

bool FAssetDocumentClassResolver::ResolveClass(const FString& ClassName, UClass*& OutClass, FString& OutError, FString& OutCode)
{
	OutCode.Reset();
	if (!ResolveClassReference(ClassName, OutClass, OutError))
	{
		OutCode = TEXT("ClassResolve.Unresolved");
		return false;
	}
	return ValidateResolvedClass(OutClass, OutError, OutCode);
}

bool FAssetDocumentClassResolver::ValidateResolvedClass(UClass* Class, FString& OutError, FString& OutCode)
{
	if (!Class)
	{
		OutError = TEXT("Failed to resolve asset class");
		OutCode = TEXT("ClassResolve.Unresolved");
		return false;
	}

	if (!Class->IsChildOf(UObject::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Resolved class '%s' is not a UObject class"), *Class->GetName());
		OutCode = TEXT("ClassResolve.Unsupported");
		return false;
	}

	if (Class->HasAnyClassFlags(CLASS_Abstract))
	{
		OutError = FString::Printf(TEXT("Resolved class '%s' is abstract; use a concrete subclass"), *Class->GetName());
		OutCode = TEXT("FactoryCreate.AbstractClass");
		return false;
	}

	if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		OutError = FString::Printf(TEXT("Resolved class '%s' is deprecated or newer-version-only"), *Class->GetName());
		OutCode = TEXT("ClassResolve.Deprecated");
		return false;
	}

	if (Class->IsChildOf(UClass::StaticClass()) ||
		Class->IsChildOf(UPackage::StaticClass()) ||
		Class->IsChildOf(UWorld::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Resolved class '%s' is not supported for asset document creation"), *Class->GetName());
		OutCode = TEXT("ClassResolve.Unsupported");
		return false;
	}

	if (Class->IsChildOf(UBlueprint::StaticClass())
		&& Class != UBlueprint::StaticClass()
		&& Class != UWidgetBlueprint::StaticClass()
		&& Class != UAnimBlueprint::StaticClass()
		&& !IsExactBlueprintClass(Class))
	{
		OutError = FString::Printf(TEXT("Resolved class '%s' is a Blueprint-derived asset class that requires an exact AssetDocument profile"), *Class->GetName());
		OutCode = TEXT("ClassResolve.ProfileRequired");
		return false;
	}

	// UMaterial itself has the Material profile; its editor-only subclasses (preview / validation
	// materials) are not assets a document creates.
	if (Class->IsChildOf(UMaterial::StaticClass()) && Class != UMaterial::StaticClass())
	{
		OutError = FString::Printf(TEXT("Resolved class '%s' is not supported by GenericAsset sidecar v1"), *Class->GetName());
		OutCode = TEXT("ClassResolve.Unsupported");
		return false;
	}

	return true;
}
