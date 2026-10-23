// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class IAssetDocumentOpenEditorHandler;

namespace UE::AssetDocument::Graph::Data
{
// Open-editor handler (Runtime/AssetDocumentOpenEditor.h, RefreshInPlace) for a UDataTable (and subclasses) open in
// the Data Table editor. The editor caches its row list and the selected row's editable copy, and refreshes them on
// FDataTableEditorUtils change notifications (which the row primitives send) and on its Undo. A rollback (package
// snapshot restore) sends none, so after the apply the handler notifies the Data Table editors of a row-list change
// (FDataTableEditorManager::PostChange, what the editor's Undo ends in), without the Blueprint-node and
// OnDataTableChanged side effects of FDataTableEditorUtils::BroadcastPostChange.
TSharedRef<IAssetDocumentOpenEditorHandler> MakeDataTableOpenEditorHandler();
} // namespace UE::AssetDocument::Graph::Data
