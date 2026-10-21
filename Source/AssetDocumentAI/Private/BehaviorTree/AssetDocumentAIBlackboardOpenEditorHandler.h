// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

// Open-editor handler (Runtime/AssetDocumentOpenEditor.h, RefreshInPlace) for a UBlackboardData. A Blackboard is
// edited in the Behavior Tree editor (opened on the Blackboard itself, or on a Behavior Tree that uses it; both list
// the Blackboard as an edited asset). Its key list (Blackboard panel and Blackboard editor) rebuilds only on its own
// key edits or on the editor's Undo (FBehaviorTreeEditor::PostUndo -> RefreshBlackboardViewsAssociatedObject), so
// after an apply the handler runs that Undo refresh on every open Behavior Tree editor that shows the Blackboard.
void RegisterAssetDocumentBlackboardOpenEditorHandler();
void UnregisterAssetDocumentBlackboardOpenEditorHandler();
