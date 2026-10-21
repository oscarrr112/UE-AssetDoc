// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

class UPackage;

namespace UE::AssetDocument::PackageSave
{
// Broadcast on the game thread right before AssetDocument saves a package, before the save looks at which of the
// package's objects it writes. A provider brings the package's objects of its types into the state their own PreSave
// leaves them in, where that state decides which objects the save writes (e.g. a reference PreSave clears).
DECLARE_MULTICAST_DELEGATE_OneParam(FOnBeforeSave, UPackage&);
ASSETDOCUMENT_API FOnBeforeSave& OnBeforeSave();
} // namespace UE::AssetDocument::PackageSave
