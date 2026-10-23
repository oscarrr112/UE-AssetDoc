// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "UObject/TopLevelAssetPath.h"

// Blueprint-derived asset classes (UBlueprint subclasses other than Blueprint, WidgetBlueprint and AnimBlueprint) are
// refused by class resolution (ClassResolve.ProfileRequired) unless a provider registers that its runtime route
// serves them with an exact profile (e.g. the Control Rig profile registers /Script/ControlRigDeveloper.
// ControlRigBlueprint). Registration is game-thread only, by class path (a module's ShutdownModule runs after the
// UObject exit purge on a normal editor exit, so it never touches the class); returns false for an empty path or a
// duplicate, and Unregister false when the path was not registered.
ASSETDOCUMENT_API bool RegisterAssetDocumentExactBlueprintClass(const FTopLevelAssetPath& BlueprintClassPath);
ASSETDOCUMENT_API bool UnregisterAssetDocumentExactBlueprintClass(const FTopLevelAssetPath& BlueprintClassPath);
// True for a registered class (exactly that class, not a subclass): such a class is created through the generic
// editor factory lifecycle like any other asset class, its profile owning the document.
ASSETDOCUMENT_API bool IsAssetDocumentExactBlueprintClass(const UClass* BlueprintClass);
