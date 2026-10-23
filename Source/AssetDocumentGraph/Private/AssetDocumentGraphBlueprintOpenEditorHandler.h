// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class IAssetDocumentOpenEditorHandler;

namespace UE::AssetDocument::Graph
{
// Open-editor handler (Runtime/AssetDocumentOpenEditor.h, RefreshInPlace) for every UBlueprint (Blueprint, Anim
// Blueprint, Widget Blueprint, ...) open in a Blueprint editor (FBlueprintEditor and its subclasses). The apply edits
// the Blueprint itself and its compile refreshes the editor. A graph the apply removed (or a rollback took away) is
// renamed out of the Blueprint, not destroyed, and its open graph tab keeps it alive, so the editor's own tab cleanup
// (CleanInvalidTabs) keeps that tab; the editor closes it itself only when its own Delete removes the graph. The
// handler closes the tabs of graphs that are no longer the Blueprint's and refreshes the editor as its Undo does
// (RefreshEditors: My Blueprint, Components tree, details, preview).
TSharedRef<IAssetDocumentOpenEditorHandler> MakeBlueprintOpenEditorHandler();
} // namespace UE::AssetDocument::Graph
