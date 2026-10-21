// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

namespace UE::AssetDocument::Graph
{
// After a successful Blueprint apply, the objects that inherit the Blueprint's class defaults
// and component templates hold the new values, exactly as after the same edit in the Blueprint
// editor. A human edits a class default (a variable's Default Value, Class Defaults, a component's
// Details) on the class default object or the component template, and the Details panel copies the
// new value into every object based on it that still holds the old value
// (FPropertyNode::PropagatePropertyChange: child Blueprint class defaults and their component
// overrides, placed and spawned instances and their components; an instance that overrides the
// value keeps it). A compile does not do this: its reinstancing keeps every instance value that
// differs from the class default object it reinstances from. The apply writes the class default
// object and the templates directly, so the same rule runs once afterwards, from a snapshot of the
// values taken before the apply: per property, a dependent whose value still exports as the old
// value gets the new value, and PostEditChangeProperty, as the Details panel calls it on archetype
// instances. Dependent packages keep their dirty state: a value equal to its archetype's is not
// saved (delta serialization), so the saved state of a dependent is unchanged.
class ASSETDOCUMENTGRAPH_API FBlueprintDefaultsSnapshot
{
public:
	// The current values of the loaded Blueprint at this object path, or unset when no Blueprint is loaded there.
	static TOptional<FBlueprintDefaultsSnapshot> Capture(const FString& BlueprintObjectPath);

	// Copies every changed value into the dependents that still hold the old value. Adds one Info
	// diagnostic (Graph.DefaultsPropagated) when anything was updated.
	void Propagate(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics) const;

	struct FValueSnapshot
	{
		FString TypeKey;
		FString Text;
	};

private:
	FString BlueprintPath;
	// Source key ("cdo", "sub:<name>", "scs:<guid>", "ich:<guid>") -> property key -> value.
	TMap<FString, TMap<FString, FValueSnapshot>> Sources;
};
}
