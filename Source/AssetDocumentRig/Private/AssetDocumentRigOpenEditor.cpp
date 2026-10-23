// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Open-editor apply of the rig profiles (Runtime/AssetDocumentOpenEditor.h):
//   ControlRig    close, apply, reopen (ControlRig.EditorReopened). The Control Rig editor edits the asset itself
//                 through the same controllers the apply uses, but it listens to every model and hierarchy
//                 notification: a rollback (the package snapshot restored by re-serializing its objects in place)
//                 runs those listeners on half-restored graphs and asserts in the engine (RigVMModelCachedValue,
//                 VersionedObject), and its debugged rig, graph tabs and hierarchy tab are only rebuilt by the editor
//                 itself. Reopening builds them from the applied (or rolled-back) rig. Opening an empty rig adds the
//                 Forwards Solve event (the editor's first open, the same event a Create adds): the human's next save
//                 would save it, so the apply accepts that open-time change (AcceptsOpenTimeChanges).
//   IKRig,        close, apply, reopen (IKRig.EditorReopened / IKRetargeter.EditorReopened). Both editors show the
//   IKRetargeter  solver, goal, bone, chain and op settings in their Details panel through struct wrappers
//                 (UIKRigStructWrapperBase): copies of the asset's structs, addressed by solver / op index, that the
//                 next Details edit writes back whole. Neither editor refreshes them on Undo, and their refresh calls
//                 (FIKRigEditorController, UIKRigStructWrapperBase::UpdateWrapperStructWithLatestValues) are not
//                 exported. Reopening rebuilds the editor, its processors and wrappers from the applied asset.

#include "AssetDocumentRigOpenEditor.h"

#include "ControlRigBlueprintLegacy.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Runtime/AssetDocumentOpenEditor.h"

namespace AssetDocumentRigOpenEditorLocal
{
class FControlRigHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentRig.ControlRig"); }
	virtual bool Supports(const UObject& Asset) const override { return Asset.IsA<UControlRigBlueprint>(); }
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("ControlRig"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::CloseApplyReopen;
	}
	virtual bool AcceptsOpenTimeChanges(const UObject& Asset) const override { return true; }
};

class FIKRigHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentRig.IKRig"); }
	virtual bool Supports(const UObject& Asset) const override { return Asset.IsA<UIKRigDefinition>() || Asset.IsA<UIKRetargeter>(); }
	virtual FString GetProfileName(const UObject& Asset) const override
	{
		return Asset.IsA<UIKRetargeter>() ? TEXT("IKRetargeter") : TEXT("IKRig");
	}
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::CloseApplyReopen;
	}
};

TArray<TSharedRef<IAssetDocumentOpenEditorHandler>>& Handlers()
{
	static TArray<TSharedRef<IAssetDocumentOpenEditorHandler>> Registered;
	return Registered;
}
} // namespace AssetDocumentRigOpenEditorLocal

void RegisterAssetDocumentRigOpenEditorHandlers()
{
	using namespace AssetDocumentRigOpenEditorLocal;
	if (!Handlers().IsEmpty())
	{
		return;
	}
	Handlers() = {MakeShared<FControlRigHandler>(), MakeShared<FIKRigHandler>()};
	for (const TSharedRef<IAssetDocumentOpenEditorHandler>& Handler : Handlers())
	{
		RegisterAssetDocumentOpenEditorHandler(Handler);
	}
}

void UnregisterAssetDocumentRigOpenEditorHandlers()
{
	using namespace AssetDocumentRigOpenEditorLocal;
	for (const TSharedRef<IAssetDocumentOpenEditorHandler>& Handler : Handlers())
	{
		UnregisterAssetDocumentOpenEditorHandler(Handler->GetId());
	}
	Handlers().Reset();
}
