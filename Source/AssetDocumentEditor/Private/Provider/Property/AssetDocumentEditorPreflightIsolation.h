// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "UObject/UObjectArray.h"

class FAssetDocumentPackageSnapshot;
class UPackage;

// Keeps a read-only probe on a transient scratch object from changing any other
// package. The preflight writes the scratch like the live
// write and the Details panel do, PostEditChange included, and that callback can
// reach real assets: e.g. a PaperSprite rebuilds its AtlasGroup, which rebuilds
// the atlas, re-bakes its sprites and dirties the atlas package.
//
// While in scope the guard watches the signals the engine raises before an object
// of a package changes: Modify (also what first dirties a package), PreEditChange
// and the PostEditChange notification (broadcast before the listeners that react to
// it; the guard subscribes last, so multicast delegates call it first). At the first
// such signal from a package other than the transient (scratch) package it captures
// a package snapshot. Restore() writes those packages back byte-exactly without
// editor callbacks (which could repeat the side effect), removes objects created in
// them and restores their original dirty flag. Objects created in a package the
// guard had not captured, with no change notification and without dirtying it, are
// the first use of content, reported as info: a package the probe loaded (its loaded
// objects create their editor data lazily) or engine/plugin content initialized on
// first use. A package the probe loaded whose objects created their editor data and
// then only sent PostEditChange (no Modify, PreEditChange or dirtying: a legacy
// skeletal mesh finishing its load-time upgrade) is a first load too, as is one whose objects
// created their editor data and initialized it with Modify without dirtying the package (a Control
// Rig module building its hierarchy on first use). Transient objects (and objects inside them) are
// never saved and are not tracked (the editor's actor factory spawns and destroys a transient
// preview actor in the editor world). Any other object created in an uncaptured package (a /Game package
// already in memory, or a package the probe created) is reported as an error that
// names the objects, the cause and the remedy. Compiled-in (/Script) packages (class
// defaults, settings) are not assets and are not tracked.
class FAssetDocumentEditorPreflightIsolation final : public FUObjectArray::FUObjectCreateListener
{
public:
	// Context names the probe in logs and diagnostics (e.g. "PaperSprite.AtlasGroup").
	explicit FAssetDocumentEditorPreflightIsolation(FString InContext);
	virtual ~FAssetDocumentEditorPreflightIsolation() override;

	FAssetDocumentEditorPreflightIsolation(const FAssetDocumentEditorPreflightIsolation&) = delete;
	FAssetDocumentEditorPreflightIsolation& operator=(const FAssetDocumentEditorPreflightIsolation&) = delete;

	// Stops watching and restores every touched package. Single use; the destructor
	// calls it when the caller did not. False when a package could not be restored.
	bool Restore(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

	// Names of the packages touched so far (for tests and logs).
	TArray<FString> GetTouchedPackageNames() const;

	virtual void NotifyUObjectCreated(const UObjectBase* Object, int32 Index) override;
	virtual void OnUObjectArrayShutdown() override;

private:
	struct FTouchedPackage;

	static FString DescribeCreatedObjects(const FTouchedPackage& Entry);
	static FString DescribeNotRestored(const FTouchedPackage& Entry, const FString& Outcome);
	void Stop();
	FTouchedPackage* Track(UObject* Object, const TCHAR* Signal);
	void Capture(FTouchedPackage& Touched, bool bBeforeChange);
	void HandleObjectModified(UObject* Object);
	void HandlePreEditChange(UObject* Object, const class FEditPropertyChain& Chain);
	void HandlePostEditChange(UObject* Object, struct FPropertyChangedEvent& Event);
	void HandlePackageDirtied(UPackage* Package, bool bWasDirty);

	FString Context;
	TArray<TUniquePtr<FTouchedPackage>> Touched;
	// Packages that appeared (were loaded or created) while watched.
	TSet<FName> PackagesAppearedInScope;
	FDelegateHandle ModifiedHandle;
	FDelegateHandle PreEditHandle;
	FDelegateHandle PostEditHandle;
	FDelegateHandle DirtyHandle;
	bool bWatching = false;
	bool bCapturing = false;
	bool bRestored = false;
};
