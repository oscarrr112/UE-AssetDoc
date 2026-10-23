// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPolicy.h"

#include "AssetDocumentPolicyRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Animation/AnimBlueprint.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintCore.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
bool FindPreset(const TArray<FAssetDocumentRegionPolicyPreset>& Presets, FName PresetName, FAssetDocumentRegionPolicyPreset& OutPreset)
{
	for (const FAssetDocumentRegionPolicyPreset& Preset : Presets)
	{
		if (Preset.PresetName == PresetName)
		{
			OutPreset = Preset;
			return true;
		}
	}
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPolicyPresetExpansionTest,
	"UEAssetDoc.Policy.PresetExpansion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPolicyPresetExpansionTest::RunTest(const FString& Parameters)
{
	const TArray<FAssetDocumentRegionPolicyPreset> BuiltinPresets = FAssetDocumentPolicyRegistry::GetBuiltinPresets();
	TestEqual(TEXT("Registry exposes three builtin presets"), BuiltinPresets.Num(), 3);

	FAssetDocumentRegionPolicyPreset DefaultDiffPreset;
	TestTrue(TEXT("GetBuiltinPresets includes DefaultDiff"), FindPreset(BuiltinPresets, TEXT("DefaultDiff"), DefaultDiffPreset));
	TestEqual(TEXT("DefaultDiff preset kind"), DefaultDiffPreset.Defaults.RegionKind, EAssetDocumentRegionKind::Object);
	TestEqual(TEXT("DefaultDiff preset source"), DefaultDiffPreset.Defaults.DefaultSource, EAssetDocumentDefaultSource::CDO);
	TestEqual(TEXT("DefaultDiff preset reducer"), DefaultDiffPreset.Defaults.ReducerMode, EAssetDocumentReducerMode::DefaultDiff);
	TestEqual(TEXT("DefaultDiff preset apply"), DefaultDiffPreset.Defaults.ApplyMode, EAssetDocumentApplyMode::SetProperty);

	FAssetDocumentRegionPolicyPreset Preset;
	TestTrue(TEXT("ManagedRegion preset is registered"), FAssetDocumentPolicyRegistry::GetBuiltinPreset(TEXT("ManagedRegion"), Preset));
	TestEqual(TEXT("ManagedRegion preset kind"), Preset.Defaults.RegionKind, EAssetDocumentRegionKind::Array);
	TestEqual(TEXT("ManagedRegion preset source"), Preset.Defaults.DefaultSource, EAssetDocumentDefaultSource::ProfileDeclared);
	TestEqual(TEXT("ManagedRegion preset reducer"), Preset.Defaults.ReducerMode, EAssetDocumentReducerMode::ManagedRegion);
	TestEqual(TEXT("ManagedRegion preset apply"), Preset.Defaults.ApplyMode, EAssetDocumentApplyMode::RebuildArrayRegion);

	FAssetDocumentRegionPolicyOverride Override;
	Override.RegionId = TEXT("Body.CompositeSections");
	Override.BodyPath = TEXT("$.Body.CompositeSections");
	Override.RegionKind = EAssetDocumentRegionKind::Timeline;
	Override.ReducerMode = EAssetDocumentReducerMode::ManagedRegion;
	Override.ApplyMode = EAssetDocumentApplyMode::RebuildArrayRegion;
	Override.ManagedUePropertyPaths = TArray<FString>{TEXT("CompositeSections")};
	Override.ExtensionHookName = TEXT("ApplyCompositeSections");

	FAssetDocumentRegionPolicy FirstPolicy;
	TestTrue(TEXT("Preset expands with an override"), FAssetDocumentPolicyRegistry::ExpandPreset(Preset, Override, FirstPolicy));

	FAssetDocumentRegionPolicy SecondPolicy;
	TestTrue(TEXT("Same preset and override expands again"), FAssetDocumentPolicyRegistry::ExpandPreset(Preset, Override, SecondPolicy));

	TestEqual(TEXT("RegionId is overridden"), FirstPolicy.RegionId, FName(TEXT("Body.CompositeSections")));
	TestEqual(TEXT("BodyPath is overridden"), FirstPolicy.BodyPath, FString(TEXT("$.Body.CompositeSections")));
	TestEqual(TEXT("RegionKind is overridden"), FirstPolicy.RegionKind, EAssetDocumentRegionKind::Timeline);
	TestEqual(TEXT("ReducerMode is overridden"), FirstPolicy.ReducerMode, EAssetDocumentReducerMode::ManagedRegion);
	TestEqual(TEXT("ApplyMode is overridden"), FirstPolicy.ApplyMode, EAssetDocumentApplyMode::RebuildArrayRegion);
	TestEqual(TEXT("ManagedUePropertyPaths is overridden"), FirstPolicy.ManagedUePropertyPaths.Num(), 1);
	if (FirstPolicy.ManagedUePropertyPaths.Num() == 1)
	{
		TestEqual(TEXT("ManagedUePropertyPaths preserves requested property path"), FirstPolicy.ManagedUePropertyPaths[0], FString(TEXT("CompositeSections")));
	}
	TestTrue(TEXT("ExtensionHookName is overridden"), FirstPolicy.ExtensionHookName.IsSet());
	if (FirstPolicy.ExtensionHookName.IsSet())
	{
		TestEqual(TEXT("ExtensionHookName preserves requested hook"), FirstPolicy.ExtensionHookName.GetValue(), FName(TEXT("ApplyCompositeSections")));
	}

	TestEqual(TEXT("Stable expansion keeps RegionId"), SecondPolicy.RegionId, FirstPolicy.RegionId);
	TestEqual(TEXT("Stable expansion keeps BodyPath"), SecondPolicy.BodyPath, FirstPolicy.BodyPath);
	TestEqual(TEXT("Stable expansion keeps ApplyMode"), SecondPolicy.ApplyMode, FirstPolicy.ApplyMode);
	TestEqual(TEXT("Stable expansion keeps ReducerMode"), SecondPolicy.ReducerMode, FirstPolicy.ReducerMode);
	TestEqual(TEXT("Stable expansion keeps ManagedUePropertyPaths count"), SecondPolicy.ManagedUePropertyPaths.Num(), FirstPolicy.ManagedUePropertyPaths.Num());

	FAssetDocumentRegionPolicyPreset ExtensionHookPreset;
	TestTrue(TEXT("ExtensionHook preset is registered"), FAssetDocumentPolicyRegistry::GetBuiltinPreset(TEXT("ExtensionHook"), ExtensionHookPreset));
	TestEqual(TEXT("ExtensionHook preset kind"), ExtensionHookPreset.Defaults.RegionKind, EAssetDocumentRegionKind::Object);
	TestEqual(TEXT("ExtensionHook preset source"), ExtensionHookPreset.Defaults.DefaultSource, EAssetDocumentDefaultSource::ProfileDeclared);
	TestEqual(TEXT("ExtensionHook preset reducer"), ExtensionHookPreset.Defaults.ReducerMode, EAssetDocumentReducerMode::ManagedRegion);
	TestEqual(TEXT("ExtensionHook preset apply"), ExtensionHookPreset.Defaults.ApplyMode, EAssetDocumentApplyMode::ExtensionHook);

	FAssetDocumentRegionPolicyOverride MissingHookOverride;
	MissingHookOverride.RegionId = TEXT("Body.Custom");
	MissingHookOverride.BodyPath = TEXT("$.Body.Custom");
	FAssetDocumentRegionPolicy MissingHookPolicy;
	TestFalse(TEXT("ExtensionHook expansion requires a hook name"), FAssetDocumentPolicyRegistry::ExpandPreset(ExtensionHookPreset, MissingHookOverride, MissingHookPolicy));

	FAssetDocumentRegionPolicyOverride NoneHookOverride = MissingHookOverride;
	NoneHookOverride.ExtensionHookName = NAME_None;
	TestFalse(TEXT("ExtensionHook expansion rejects NAME_None hook"), FAssetDocumentPolicyRegistry::ExpandPreset(ExtensionHookPreset, NoneHookOverride, MissingHookPolicy));

	FAssetDocumentRegionPolicyPreset InvalidPreset;
	TestFalse(TEXT("Invalid preset name is not registered"), FAssetDocumentPolicyRegistry::GetBuiltinPreset(TEXT("DefinitelyMissing"), InvalidPreset));
	TestFalse(TEXT("Invalid preset does not expand"), FAssetDocumentPolicyRegistry::ExpandPreset(InvalidPreset, Override, MissingHookPolicy));

	FAssetDocumentRegionPolicyPreset CustomPreset;
	CustomPreset.PresetName = TEXT("CustomInherited");
	CustomPreset.Defaults.RegionId = TEXT("Body.Inherited");
	CustomPreset.Defaults.BodyPath = TEXT("$.Body.Inherited");
	CustomPreset.Defaults.ManagedUePropertyPaths.Add(TEXT("InheritedProperty"));
	CustomPreset.Defaults.ExtractOnlyFields.Add(TEXT("_ProjectionMetrics"));
	CustomPreset.Defaults.ExplicitDeleteValues.Add(TEXT("None"));

	FAssetDocumentIdentityRule InheritedIdentityRule;
	InheritedIdentityRule.FieldPath = TEXT("Items");
	InheritedIdentityRule.UePropertyPath = TEXT("Items");
	InheritedIdentityRule.StableKeyField = TEXT("Name");
	CustomPreset.Defaults.IdentityRules.Add(InheritedIdentityRule);

	FAssetDocumentComparisonRule InheritedComparisonRule;
	InheritedComparisonRule.FieldPath = TEXT("Items");
	InheritedComparisonRule.ComparatorName = TEXT("StableArray");
	InheritedComparisonRule.bIgnoreOrder = true;
	CustomPreset.Defaults.ComparisonRules.Add(InheritedComparisonRule);

	FAssetDocumentRegionPolicyOverride ClearOverride;
	ClearOverride.RegionId = TEXT("Body.Cleared");
	ClearOverride.BodyPath = TEXT("$.Body.Cleared");
	ClearOverride.ManagedUePropertyPaths = TArray<FString>();
	ClearOverride.ExtractOnlyFields = TSet<FString>();
	ClearOverride.ExplicitDeleteValues = TSet<FString>();
	ClearOverride.IdentityRules = TArray<FAssetDocumentIdentityRule>();
	ClearOverride.ComparisonRules = TArray<FAssetDocumentComparisonRule>();

	FAssetDocumentRegionPolicy ClearedPolicy;
	TestTrue(TEXT("Custom preset expands with explicit clear overrides"), FAssetDocumentPolicyRegistry::ExpandPreset(CustomPreset, ClearOverride, ClearedPolicy));
	TestEqual(TEXT("Explicit clear removes ManagedUePropertyPaths"), ClearedPolicy.ManagedUePropertyPaths.Num(), 0);
	TestEqual(TEXT("Explicit clear removes ExtractOnlyFields"), ClearedPolicy.ExtractOnlyFields.Num(), 0);
	TestEqual(TEXT("Explicit clear removes ExplicitDeleteValues"), ClearedPolicy.ExplicitDeleteValues.Num(), 0);
	TestEqual(TEXT("Explicit clear removes IdentityRules"), ClearedPolicy.IdentityRules.Num(), 0);
	TestEqual(TEXT("Explicit clear removes ComparisonRules"), ClearedPolicy.ComparisonRules.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPolicyJsonExportTest,
	"UEAssetDoc.Policy.JsonExport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPolicyJsonExportTest::RunTest(const FString& Parameters)
{
	FAssetDocumentRegionPolicy Policy;
	Policy.RegionId = TEXT("Body.Blend");
	Policy.BodyPath = TEXT("$.Body.Blend");
	Policy.RegionKind = EAssetDocumentRegionKind::Object;
	Policy.DefaultSource = EAssetDocumentDefaultSource::CDO;
	Policy.ReducerMode = EAssetDocumentReducerMode::DefaultDiff;
	Policy.ApplyMode = EAssetDocumentApplyMode::SetProperty;
	Policy.ManagedUePropertyPaths = {TEXT("BlendIn"), TEXT("BlendOut")};
	Policy.IdentityRules.Add(FAssetDocumentIdentityRule());
	FAssetDocumentIdentityRule IdentityRule;
	IdentityRule.FieldPath = TEXT("CompositeSections");
	IdentityRule.UePropertyPath = TEXT("CompositeSections");
	IdentityRule.StableKeyField = TEXT("SectionName");
	Policy.IdentityRules.Add(IdentityRule);

	Policy.ComparisonRules.Add(FAssetDocumentComparisonRule());
	FAssetDocumentComparisonRule ComparisonRule;
	ComparisonRule.FieldPath = TEXT("SlotAnimTracks");
	ComparisonRule.ComparatorName = TEXT("StableArray");
	ComparisonRule.bIgnoreOrder = true;
	Policy.ComparisonRules.Add(ComparisonRule);

	const TSharedRef<FJsonObject> Json = FAssetDocumentPolicyRegistry::ExportPolicyToJson(Policy);

	TestEqual(TEXT("JSON includes RegionId"), Json->GetStringField(TEXT("RegionId")), FString(TEXT("Body.Blend")));
	TestEqual(TEXT("JSON includes BodyPath"), Json->GetStringField(TEXT("BodyPath")), FString(TEXT("$.Body.Blend")));
	TestEqual(TEXT("JSON includes RegionKind"), Json->GetStringField(TEXT("RegionKind")), FString(TEXT("Object")));
	TestEqual(TEXT("JSON includes DefaultSource"), Json->GetStringField(TEXT("DefaultSource")), FString(TEXT("CDO")));
	TestEqual(TEXT("JSON includes ReducerMode"), Json->GetStringField(TEXT("ReducerMode")), FString(TEXT("DefaultDiff")));
	TestEqual(TEXT("JSON includes ApplyMode"), Json->GetStringField(TEXT("ApplyMode")), FString(TEXT("SetProperty")));

	const TArray<TSharedPtr<FJsonValue>>* ManagedPaths = nullptr;
	TestTrue(TEXT("JSON includes non-empty ManagedUePropertyPaths"), Json->TryGetArrayField(TEXT("ManagedUePropertyPaths"), ManagedPaths));
	if (ManagedPaths)
	{
		TestEqual(TEXT("JSON exports all managed paths"), ManagedPaths->Num(), 2);
	}

	const TArray<TSharedPtr<FJsonValue>>* IdentityRules = nullptr;
	TestTrue(TEXT("JSON includes non-empty IdentityRules"), Json->TryGetArrayField(TEXT("IdentityRules"), IdentityRules));
	if (IdentityRules)
	{
		TestEqual(TEXT("JSON filters empty identity rules"), IdentityRules->Num(), 1);
		if (IdentityRules->Num() == 1)
		{
			const TSharedPtr<FJsonObject> RuleObject = (*IdentityRules)[0]->AsObject();
			TestTrue(TEXT("Identity rule exports as object"), RuleObject.IsValid());
			if (RuleObject.IsValid())
			{
				TestEqual(TEXT("Identity rule exports FieldPath"), RuleObject->GetStringField(TEXT("FieldPath")), FString(TEXT("CompositeSections")));
				TestEqual(TEXT("Identity rule exports StableKeyField"), RuleObject->GetStringField(TEXT("StableKeyField")), FString(TEXT("SectionName")));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ComparisonRules = nullptr;
	TestTrue(TEXT("JSON includes non-empty ComparisonRules"), Json->TryGetArrayField(TEXT("ComparisonRules"), ComparisonRules));
	if (ComparisonRules)
	{
		TestEqual(TEXT("JSON filters empty comparison rules"), ComparisonRules->Num(), 1);
		if (ComparisonRules->Num() == 1)
		{
			const TSharedPtr<FJsonObject> RuleObject = (*ComparisonRules)[0]->AsObject();
			TestTrue(TEXT("Comparison rule exports as object"), RuleObject.IsValid());
			if (RuleObject.IsValid())
			{
				TestEqual(TEXT("Comparison rule exports FieldPath"), RuleObject->GetStringField(TEXT("FieldPath")), FString(TEXT("SlotAnimTracks")));
				TestEqual(TEXT("Comparison rule exports ComparatorName"), RuleObject->GetStringField(TEXT("ComparatorName")), FString(TEXT("StableArray")));
				TestTrue(TEXT("Comparison rule exports IgnoreOrder"), RuleObject->GetBoolField(TEXT("IgnoreOrder")));
			}
		}
	}

	TestFalse(TEXT("JSON omits empty ExtractOnlyFields"), Json->HasField(TEXT("ExtractOnlyFields")));
	TestFalse(TEXT("JSON omits empty ExplicitDeleteValues"), Json->HasField(TEXT("ExplicitDeleteValues")));
	TestFalse(TEXT("JSON omits unset ExtensionHookName"), Json->HasField(TEXT("ExtensionHookName")));
	TestFalse(TEXT("JSON omits unset CanonicalizerHookName"), Json->HasField(TEXT("CanonicalizerHookName")));
	TestFalse(TEXT("JSON omits preset expansion scratch fields"), Json->HasField(TEXT("PresetName")));

	FAssetDocumentRegionPolicyPreset Preset;
	TestTrue(TEXT("ManagedRegion preset exists for JSON export"), FAssetDocumentPolicyRegistry::GetBuiltinPreset(TEXT("ManagedRegion"), Preset));
	const TSharedRef<FJsonObject> PresetJson = FAssetDocumentPolicyRegistry::ExportPresetToJson(Preset);
	TestEqual(TEXT("Preset JSON includes name"), PresetJson->GetStringField(TEXT("PresetName")), FString(TEXT("ManagedRegion")));
	TestEqual(TEXT("Preset JSON includes version"), static_cast<int32>(PresetJson->GetNumberField(TEXT("PolicyVersion"))), 1);

	const TSharedPtr<FJsonObject>* Defaults = nullptr;
	TestTrue(TEXT("Preset JSON includes Defaults"), PresetJson->TryGetObjectField(TEXT("Defaults"), Defaults));
	if (Defaults && Defaults->IsValid())
	{
		TestEqual(TEXT("Preset defaults export RegionKind"), (*Defaults)->GetStringField(TEXT("RegionKind")), FString(TEXT("Array")));
		TestEqual(TEXT("Preset defaults export ReducerMode"), (*Defaults)->GetStringField(TEXT("ReducerMode")), FString(TEXT("ManagedRegion")));
		TestFalse(TEXT("Preset defaults omit empty RegionId"), (*Defaults)->HasField(TEXT("RegionId")));
		TestFalse(TEXT("Preset defaults omit empty BodyPath"), (*Defaults)->HasField(TEXT("BodyPath")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPolicyCanonicalizerHookExpandsAndExportsTest,
	"UEAssetDoc.Policy.CanonicalizerHookExpandsAndExports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPolicyCanonicalizerHookExpandsAndExportsTest::RunTest(const FString& Parameters)
{
	FAssetDocumentRegionPolicyPreset Preset;
	TestTrue(TEXT("ManagedRegion preset exists"), FindPreset(FAssetDocumentPolicyRegistry::GetBuiltinPresets(), TEXT("ManagedRegion"), Preset));

	FAssetDocumentRegionPolicyOverride Override;
	Override.RegionId = TEXT("Body.UbergraphPages");
	Override.BodyPath = TEXT("Body.UbergraphPages");
	Override.CanonicalizerHookName = TEXT("UBlueprintGraph");

	FAssetDocumentRegionPolicy Policy;
	TestTrue(TEXT("Preset expands with canonicalizer hook"), FAssetDocumentPolicyRegistry::ExpandPreset(Preset, Override, Policy));
	TestEqual(TEXT("Expanded policy stores canonicalizer hook"), Policy.CanonicalizerHookName, FName(TEXT("UBlueprintGraph")));

	const TSharedRef<FJsonObject> Exported = FAssetDocumentPolicyRegistry::ExportPolicyToJson(Policy);
	TestEqual(TEXT("Exported policy includes canonicalizer hook"), Exported->GetStringField(TEXT("CanonicalizerHookName")), FString(TEXT("UBlueprintGraph")));

	return true;
}

#endif
