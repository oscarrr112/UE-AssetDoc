// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "CoreMinimal.h"

class UClass;
class UFactory;
class UObject;

// The one place where the Editor provider keeps an MCP request from blocking on,
// or crashing in, editor code that was written for an interactive user (modal
// dialogs, pickers, asserts on missing user input):
//   1. FAssetDocumentEditorNonInteractiveScope (public, AssetDocumentEditorNonInteractiveScope.h):
//      every factory call runs inside it, so a factory can never open a modal window.
//   2. Factory requirements: factories whose FactoryCreateNew dereferences a
//      property that only their New-asset dialog (ConfigureProperties) sets.
//   3. Scratch clone and change-notification preconditions for classes whose
//      editor callbacks crash outside a normal interactive editor.

// Factories whose FactoryCreateNew uses a property that only their New-asset
// dialog sets (LightWeightInstance ParentClass, SlateWidgetStyle StyleType,
// Skeleton source meshes). Returns true when the factory can run; otherwise
// OutCode = FactoryCreate.MissingFactoryContext and OutMessage says what the
// factory needs. A factory_context configured onto the property satisfies it.
bool CheckAssetDocumentEditorFactoryRequirements(
	const UFactory& Factory, const UClass& AssetClass, FString& OutCode, FString& OutMessage);

// Scratch clones are transient non-asset duplicates. Classes whose duplication
// callbacks require a real asset (MetaSound documents assert IsAsset() when
// their builder registers the duplicate) cannot be previewed that way.
bool CheckAssetDocumentEditorScratchCloneSupported(const UObject& Source, FString& OutMessage);

// Property writes notify the object (PostEditChange). Classes whose notification
// dereferences an editor service that may be absent (SoundEffectSourcePresetChain
// needs the audio device manager, which -NoSound / headless editors lack) are
// refused while that service is missing. OutCode = PropertyWrite.EditorServiceUnavailable.
bool CheckAssetDocumentEditorChangeNotificationSupported(
	const UObject& Object, FString& OutCode, FString& OutMessage);
