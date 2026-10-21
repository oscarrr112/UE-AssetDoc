// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "Misc/OutputDevice.h"
#include "Templates/Function.h"
#include "Templates/UnrealTemplate.h"
#include <atomic>
#include "UObject/UObjectArray.h"

class UClass;

// Public so that providers outside the Editor module (the data provider's Details-panel
// ParentTables edit) run editor code that may raise a dialog under the same guard as the
// Editor provider's factory calls (AssetDocumentEditorEditorSafety.h).

inline constexpr const TCHAR* AssetDocumentEditorInteractiveFactoryCode =
	TEXT("FactoryCreate.InteractiveFactory");

// Runs editor code as an unattended script for its lifetime:
// GIsRunningUnattendedScript makes FSlateApplication::AddModalWindow cancel the
// window before it is created (the engine logs "A modal window tried to take
// control while running in unattended script mode") and FMessageDialog::Open
// return its default answer. The scope records any modal attempt (that log line,
// the editor's pre-Slate-modal broadcast, or a modal loop that still started, whose
// window it closes on the first loop tick) and every UPackage created while it
// is open, so a refused factory call can be rolled back without residue.
// A slow task's progress window (FScopedSlowTask::MakeDialog, opened by material
// recompiles among others) is not refused: it never blocks and asks nothing.
//
// Not covered (no engine hook without an Engine change): native OS dialogs
// (FDesktopPlatform file pickers, FPlatformMisc::MessageBoxExt called directly),
// and code that blocks the game thread without any UI. Such factories go in the
// factory requirement list instead.
class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorNonInteractiveScope final
	: public FOutputDevice
	, public FUObjectArray::FUObjectCreateListener
{
public:
	FAssetDocumentEditorNonInteractiveScope();
	virtual ~FAssetDocumentEditorNonInteractiveScope() override;

	FAssetDocumentEditorNonInteractiveScope(const FAssetDocumentEditorNonInteractiveScope&) = delete;
	FAssetDocumentEditorNonInteractiveScope& operator=(const FAssetDocumentEditorNonInteractiveScope&) = delete;

	bool WasInteractionRefused() const;

	// Discards every never-saved package created while the scope was open (the
	// editor's own discard of an unwanted in-memory package). Call it only after
	// the scope's work failed; returns false if one could not be discarded.
	bool DiscardCreatedPackages();

	// "FactoryCreate.InteractiveFactory" message for a refused factory call.
	static FString DescribeRefusal(const UClass* FactoryClass, const UClass* AssetClass);

	// Adds / removes a handler of the broadcast the editor makes before it shows a Slate modal window (a slow task's
	// progress window included).
	static FDelegateHandle AddPreSlateModalHandler(TFunction<void()> Handler);
	static void RemovePreSlateModalHandler(FDelegateHandle Handle);

	// FOutputDevice
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	// FUObjectArray::FUObjectCreateListener
	virtual void NotifyUObjectCreated(const class UObjectBase* Object, int32 Index) override;
	virtual void OnUObjectArrayShutdown() override;

private:
	void Detach();
	void OnModalLoopTick(float DeltaTime);

	TGuardValue<bool> UnattendedScript;
	std::atomic<bool> bInteractionRefused{false};
	FCriticalSection CreatedLock;
	TArray<int32> CreatedPackageIndices;
	FDelegateHandle PreSlateModalHandle;
	FDelegateHandle ModalLoopTickHandle;
	bool bAttached = false;
};
