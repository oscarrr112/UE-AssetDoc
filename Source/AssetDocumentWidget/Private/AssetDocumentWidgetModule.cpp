// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGraphAdapters.h"
#include "AssetDocumentRuntimeModule.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Widget.h"
#include "Modules/ModuleManager.h"
#include "WidgetBlueprint.h"

namespace
{
// A Widget Blueprint's package is restored by the graph module's rebuilder: widget tree and
// animations are authored objects restored from the snapshot, and the compile regenerates
// their class-side copies.
class FWidgetBlueprintRebuildAdapter final : public IAssetDocumentBlueprintRebuildAdapter
{
public:
	virtual UClass* GetBlueprintClass() const override { return UWidgetBlueprint::StaticClass(); }

	virtual bool IsDerivedObject(const UObject& Object) const override
	{
		// The widget-tree archetype and *_INST animations under a widget generated class are
		// duplicated from the authored tree/animations by every compile.
		for (const UObject* Current = Object.GetOuter(); Current != nullptr && !Current->IsA<UPackage>(); Current = Current->GetOuter())
		{
			if (Current->IsA<UWidgetBlueprintGeneratedClass>())
			{
				return true;
			}
		}
		return false;
	}
};

// Widget variables get bound events too (the Details panel's "On Clicked [+]" in a Widget Blueprint).
class FWidgetBoundEventSourceAdapter final : public IAssetDocumentBoundEventSourceAdapter
{
public:
	virtual UClass* GetBlueprintClass() const override { return UWidgetBlueprint::StaticClass(); }
	virtual UClass* GetBoundObjectClass() const override { return UWidget::StaticClass(); }
};
} // namespace

class FAssetDocumentWidgetModule final : public IAssetDocumentRuntimeModule
{
public:
	virtual void StartupModule() override
	{
		// Widget Blueprint hooks of the generic graph layer (contract graph-primitives §5);
		// the "umg" provider itself is assembled by the runtime bootstrap.
		RebuildAdapter = MakeShared<FWidgetBlueprintRebuildAdapter>();
		RegisterAssetDocumentBlueprintRebuildAdapter(RebuildAdapter.ToSharedRef());
		BoundEventSource = MakeShared<FWidgetBoundEventSourceAdapter>();
		RegisterAssetDocumentBoundEventSourceAdapter(BoundEventSource.ToSharedRef());
	}

	virtual void ShutdownModule() override
	{
		if (BoundEventSource.IsValid())
		{
			UnregisterAssetDocumentBoundEventSourceAdapter(BoundEventSource.ToSharedRef());
		}
		if (RebuildAdapter.IsValid())
		{
			UnregisterAssetDocumentBlueprintRebuildAdapter(RebuildAdapter.ToSharedRef());
		}
		BoundEventSource.Reset();
		RebuildAdapter.Reset();
	}

	virtual FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentWidget"); }

private:
	TSharedPtr<IAssetDocumentBlueprintRebuildAdapter> RebuildAdapter;
	TSharedPtr<IAssetDocumentBoundEventSourceAdapter> BoundEventSource;
};

IMPLEMENT_MODULE(FAssetDocumentWidgetModule, AssetDocumentWidget)
