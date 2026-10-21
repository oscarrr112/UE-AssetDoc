// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply of Physics Assets (Runtime/AssetDocumentOpenEditor.h, design §P3): close, apply, reopen
// (PhysicsAsset.EditorReopened). The Physics Asset Editor keeps its selection as body / constraint / shape indices, a
// preview component with physics state built from the bodies, convex meshes, the overlapping body pairs and its
// skeleton tree's physics items in FPhysicsAssetEditorSharedData; it rebuilds them after its own Undo
// (FPhysicsAssetEditor::PostUndo: SharedData->PostUndo, RefreshPhysicsAssetChange), but that class is private to the
// editor module and has no public refresh. An apply that adds or removes bodies would leave the selection pointing at
// other or missing bodies and the preview on the old ones. Reopening rebuilds all of it from the applied asset (the
// selection is cleared, as the editor's PostUndo does when it no longer fits; a running simulation stops).

#include "AssetDocumentPhysicsPrimitives.h"

#include "PhysicsEngine/PhysicsAsset.h"
#include "Runtime/AssetDocumentOpenEditor.h"

namespace AssetDocumentPhysicsOpenEditorLocal
{
class FPhysicsAssetHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentPhysics.PhysicsAsset"); }
	virtual bool Supports(const UObject& Asset) const override { return Asset.IsA<UPhysicsAsset>(); }
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("PhysicsAsset"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::CloseApplyReopen;
	}
};
} // namespace AssetDocumentPhysicsOpenEditorLocal

namespace UE::AssetDocument::Graph::Physics
{
TSharedRef<IAssetDocumentOpenEditorHandler> MakeOpenEditorHandler()
{
	return MakeShared<AssetDocumentPhysicsOpenEditorLocal::FPhysicsAssetHandler>();
}
} // namespace UE::AssetDocument::Graph::Physics
