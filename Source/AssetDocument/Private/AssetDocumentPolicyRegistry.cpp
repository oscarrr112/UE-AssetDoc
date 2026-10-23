// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPolicyRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

namespace
{
const TCHAR* ToString(EAssetDocumentRegionKind Value)
{
	switch (Value)
	{
	case EAssetDocumentRegionKind::Scalar:
		return TEXT("Scalar");
	case EAssetDocumentRegionKind::Object:
		return TEXT("Object");
	case EAssetDocumentRegionKind::Array:
		return TEXT("Array");
	case EAssetDocumentRegionKind::Timeline:
		return TEXT("Timeline");
	case EAssetDocumentRegionKind::Graph:
		return TEXT("Graph");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* ToString(EAssetDocumentDefaultSource Value)
{
	switch (Value)
	{
	case EAssetDocumentDefaultSource::CDO:
		return TEXT("CDO");
	case EAssetDocumentDefaultSource::EmptyTemplate:
		return TEXT("EmptyTemplate");
	case EAssetDocumentDefaultSource::CurrentAssetBaseline:
		return TEXT("CurrentAssetBaseline");
	case EAssetDocumentDefaultSource::ProfileDeclared:
		return TEXT("ProfileDeclared");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* ToString(EAssetDocumentReducerMode Value)
{
	switch (Value)
	{
	case EAssetDocumentReducerMode::DefaultDiff:
		return TEXT("DefaultDiff");
	case EAssetDocumentReducerMode::ManagedRegion:
		return TEXT("ManagedRegion");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* ToString(EAssetDocumentApplyMode Value)
{
	switch (Value)
	{
	case EAssetDocumentApplyMode::SetProperty:
		return TEXT("SetProperty");
	case EAssetDocumentApplyMode::RebuildArrayRegion:
		return TEXT("RebuildArrayRegion");
	case EAssetDocumentApplyMode::ExtensionHook:
		return TEXT("ExtensionHook");
	default:
		return TEXT("Unknown");
	}
}

bool IsEmptyRule(const FAssetDocumentIdentityRule& Rule)
{
	return Rule.FieldPath.IsEmpty()
		&& Rule.UePropertyPath.IsEmpty()
		&& Rule.StableKeyField.IsEmpty();
}

bool IsEmptyRule(const FAssetDocumentComparisonRule& Rule)
{
	return Rule.FieldPath.IsEmpty()
		&& Rule.ComparatorName.IsEmpty()
		&& !Rule.bIgnoreOrder;
}

TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TSet<FString>& Values)
{
	TArray<FString> SortedValues = Values.Array();
	SortedValues.Sort();
	return MakeStringArray(SortedValues);
}

TSharedRef<FJsonObject> IdentityRuleToJson(const FAssetDocumentIdentityRule& Rule)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Rule.FieldPath.IsEmpty())
	{
		Json->SetStringField(TEXT("FieldPath"), Rule.FieldPath);
	}
	if (!Rule.UePropertyPath.IsEmpty())
	{
		Json->SetStringField(TEXT("UePropertyPath"), Rule.UePropertyPath);
	}
	if (!Rule.StableKeyField.IsEmpty())
	{
		Json->SetStringField(TEXT("StableKeyField"), Rule.StableKeyField);
	}
	return Json;
}

TSharedRef<FJsonObject> ComparisonRuleToJson(const FAssetDocumentComparisonRule& Rule)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Rule.FieldPath.IsEmpty())
	{
		Json->SetStringField(TEXT("FieldPath"), Rule.FieldPath);
	}
	if (!Rule.ComparatorName.IsEmpty())
	{
		Json->SetStringField(TEXT("ComparatorName"), Rule.ComparatorName);
	}
	if (Rule.bIgnoreOrder)
	{
		Json->SetBoolField(TEXT("IgnoreOrder"), true);
	}
	return Json;
}

TArray<TSharedPtr<FJsonValue>> MakeIdentityRuleArray(const TArray<FAssetDocumentIdentityRule>& Rules)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Rules.Num());
	for (const FAssetDocumentIdentityRule& Rule : Rules)
	{
		if (IsEmptyRule(Rule))
		{
			continue;
		}
		Result.Add(MakeShared<FJsonValueObject>(IdentityRuleToJson(Rule)));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> MakeComparisonRuleArray(const TArray<FAssetDocumentComparisonRule>& Rules)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Rules.Num());
	for (const FAssetDocumentComparisonRule& Rule : Rules)
	{
		if (IsEmptyRule(Rule))
		{
			continue;
		}
		Result.Add(MakeShared<FJsonValueObject>(ComparisonRuleToJson(Rule)));
	}
	return Result;
}

FAssetDocumentRegionPolicyPreset MakeDefaultDiffPreset()
{
	FAssetDocumentRegionPolicyPreset Preset;
	Preset.PresetName = TEXT("DefaultDiff");
	Preset.PolicyVersion = 1;
	Preset.Defaults.RegionKind = EAssetDocumentRegionKind::Object;
	Preset.Defaults.DefaultSource = EAssetDocumentDefaultSource::CDO;
	Preset.Defaults.ReducerMode = EAssetDocumentReducerMode::DefaultDiff;
	Preset.Defaults.ApplyMode = EAssetDocumentApplyMode::SetProperty;
	return Preset;
}

FAssetDocumentRegionPolicyPreset MakeManagedRegionPreset()
{
	FAssetDocumentRegionPolicyPreset Preset;
	Preset.PresetName = TEXT("ManagedRegion");
	Preset.PolicyVersion = 1;
	Preset.Defaults.RegionKind = EAssetDocumentRegionKind::Array;
	Preset.Defaults.DefaultSource = EAssetDocumentDefaultSource::ProfileDeclared;
	Preset.Defaults.ReducerMode = EAssetDocumentReducerMode::ManagedRegion;
	Preset.Defaults.ApplyMode = EAssetDocumentApplyMode::RebuildArrayRegion;
	return Preset;
}

FAssetDocumentRegionPolicyPreset MakeExtensionHookPreset()
{
	FAssetDocumentRegionPolicyPreset Preset;
	Preset.PresetName = TEXT("ExtensionHook");
	Preset.PolicyVersion = 1;
	Preset.Defaults.RegionKind = EAssetDocumentRegionKind::Object;
	Preset.Defaults.DefaultSource = EAssetDocumentDefaultSource::ProfileDeclared;
	Preset.Defaults.ReducerMode = EAssetDocumentReducerMode::ManagedRegion;
	Preset.Defaults.ApplyMode = EAssetDocumentApplyMode::ExtensionHook;
	return Preset;
}
}

FString FAssetDocumentAuthoredSurfacePropertyRule::GetPropertyPath() const
{
	return DeclaringType.ToString() + TEXT(".") + PropertyName.ToString();
}

bool FAssetDocumentPolicyRegistry::GetBuiltinPreset(FName PresetName, FAssetDocumentRegionPolicyPreset& OutPreset)
{
	for (const FAssetDocumentRegionPolicyPreset& Preset : GetBuiltinPresets())
	{
		if (Preset.PresetName == PresetName)
		{
			OutPreset = Preset;
			return true;
		}
	}
	return false;
}

TArray<FAssetDocumentRegionPolicyPreset> FAssetDocumentPolicyRegistry::GetBuiltinPresets()
{
	return {
		MakeDefaultDiffPreset(),
		MakeManagedRegionPreset(),
		MakeExtensionHookPreset()
	};
}

bool FAssetDocumentPolicyRegistry::ExpandPreset(
	const FAssetDocumentRegionPolicyPreset& Preset,
	const FAssetDocumentRegionPolicyOverride& Override,
	FAssetDocumentRegionPolicy& OutPolicy)
{
	if (Preset.PresetName.IsNone())
	{
		return false;
	}

	OutPolicy = Preset.Defaults;
	if (Override.RegionId.IsSet())
	{
		OutPolicy.RegionId = Override.RegionId.GetValue();
	}
	if (Override.BodyPath.IsSet())
	{
		OutPolicy.BodyPath = Override.BodyPath.GetValue();
	}
	if (Override.RegionKind.IsSet())
	{
		OutPolicy.RegionKind = Override.RegionKind.GetValue();
	}
	if (Override.DefaultSource.IsSet())
	{
		OutPolicy.DefaultSource = Override.DefaultSource.GetValue();
	}
	if (Override.ReducerMode.IsSet())
	{
		OutPolicy.ReducerMode = Override.ReducerMode.GetValue();
	}
	if (Override.ApplyMode.IsSet())
	{
		OutPolicy.ApplyMode = Override.ApplyMode.GetValue();
	}
	if (Override.IdentityRules.IsSet())
	{
		OutPolicy.IdentityRules = Override.IdentityRules.GetValue();
	}
	if (Override.ComparisonRules.IsSet())
	{
		OutPolicy.ComparisonRules = Override.ComparisonRules.GetValue();
	}
	if (Override.ManagedUePropertyPaths.IsSet())
	{
		OutPolicy.ManagedUePropertyPaths = Override.ManagedUePropertyPaths.GetValue();
	}
	if (Override.ExtractOnlyFields.IsSet())
	{
		OutPolicy.ExtractOnlyFields = Override.ExtractOnlyFields.GetValue();
	}
	if (Override.ExplicitDeleteValues.IsSet())
	{
		OutPolicy.ExplicitDeleteValues = Override.ExplicitDeleteValues.GetValue();
	}
	if (Override.ExtensionHookName.IsSet())
	{
		OutPolicy.ExtensionHookName = Override.ExtensionHookName.GetValue();
	}
	if (Override.CanonicalizerHookName.IsSet())
	{
		OutPolicy.CanonicalizerHookName = Override.CanonicalizerHookName.GetValue();
	}
	if (OutPolicy.ApplyMode == EAssetDocumentApplyMode::ExtensionHook
		&& (!OutPolicy.ExtensionHookName.IsSet() || OutPolicy.ExtensionHookName.GetValue().IsNone()))
	{
		return false;
	}

	return !OutPolicy.RegionId.IsNone() && !OutPolicy.BodyPath.IsEmpty();
}

bool FAssetDocumentPolicyRegistry::MakeBuiltinRegionPolicy(
	FName PresetName,
	FName RegionId,
	EAssetDocumentRegionKind RegionKind,
	TArray<FString> ManagedUePropertyPaths,
	FAssetDocumentRegionPolicy& OutPolicy,
	FName CanonicalizerHookName)
{
	FAssetDocumentRegionPolicyPreset Preset;
	if (!GetBuiltinPreset(PresetName, Preset))
	{
		return false;
	}

	FAssetDocumentRegionPolicyOverride Override;
	Override.RegionId = RegionId;
	Override.BodyPath = RegionId.ToString();
	Override.RegionKind = RegionKind;
	if (!ManagedUePropertyPaths.IsEmpty())
	{
		Override.ManagedUePropertyPaths = MoveTemp(ManagedUePropertyPaths);
	}
	if (!CanonicalizerHookName.IsNone())
	{
		Override.CanonicalizerHookName = CanonicalizerHookName;
	}
	return ExpandPreset(Preset, Override, OutPolicy);
}

TSharedRef<FJsonObject> FAssetDocumentPolicyRegistry::ExportPolicyToJson(const FAssetDocumentRegionPolicy& Policy)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Policy.RegionId.IsNone())
	{
		Json->SetStringField(TEXT("RegionId"), Policy.RegionId.ToString());
	}
	if (!Policy.BodyPath.IsEmpty())
	{
		Json->SetStringField(TEXT("BodyPath"), Policy.BodyPath);
	}
	Json->SetStringField(TEXT("RegionKind"), ToString(Policy.RegionKind));
	Json->SetStringField(TEXT("DefaultSource"), ToString(Policy.DefaultSource));
	Json->SetStringField(TEXT("ReducerMode"), ToString(Policy.ReducerMode));
	Json->SetStringField(TEXT("ApplyMode"), ToString(Policy.ApplyMode));

	if (Policy.IdentityRules.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> IdentityRules = MakeIdentityRuleArray(Policy.IdentityRules);
		if (IdentityRules.Num() > 0)
		{
			Json->SetArrayField(TEXT("IdentityRules"), MoveTemp(IdentityRules));
		}
	}
	if (Policy.ComparisonRules.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ComparisonRules = MakeComparisonRuleArray(Policy.ComparisonRules);
		if (ComparisonRules.Num() > 0)
		{
			Json->SetArrayField(TEXT("ComparisonRules"), MoveTemp(ComparisonRules));
		}
	}
	if (Policy.ManagedUePropertyPaths.Num() > 0)
	{
		Json->SetArrayField(TEXT("ManagedUePropertyPaths"), MakeStringArray(Policy.ManagedUePropertyPaths));
	}
	if (Policy.ExtractOnlyFields.Num() > 0)
	{
		Json->SetArrayField(TEXT("ExtractOnlyFields"), MakeStringArray(Policy.ExtractOnlyFields));
	}
	if (Policy.ExplicitDeleteValues.Num() > 0)
	{
		Json->SetArrayField(TEXT("ExplicitDeleteValues"), MakeStringArray(Policy.ExplicitDeleteValues));
	}
	if (Policy.ExtensionHookName.IsSet())
	{
		Json->SetStringField(TEXT("ExtensionHookName"), Policy.ExtensionHookName.GetValue().ToString());
	}
	if (!Policy.CanonicalizerHookName.IsNone())
	{
		Json->SetStringField(TEXT("CanonicalizerHookName"), Policy.CanonicalizerHookName.ToString());
	}

	return Json;
}

TSharedRef<FJsonObject> FAssetDocumentPolicyRegistry::ExportPresetToJson(const FAssetDocumentRegionPolicyPreset& Preset)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("PresetName"), Preset.PresetName.ToString());
	Json->SetNumberField(TEXT("PolicyVersion"), Preset.PolicyVersion);
	Json->SetObjectField(TEXT("Defaults"), ExportPolicyToJson(Preset.Defaults));
	return Json;
}

int32 FAssetDocumentPolicyRegistry::CountMatchingAuthoredSurfaceRules(
	const FProperty& Property,
	const TArray<FAssetDocumentAuthoredSurfacePropertyRule>& Rules,
	const FAssetDocumentAuthoredSurfacePropertyRule** OutRule)
{
	if (OutRule)
	{
		*OutRule = nullptr;
	}

	const UStruct* DeclaringType = Property.GetOwnerStruct();
	const FName DeclaringTypeName = DeclaringType ? DeclaringType->GetFName() : NAME_None;
	int32 MatchCount = 0;
	for (const FAssetDocumentAuthoredSurfacePropertyRule& Rule : Rules)
	{
		if (Rule.DeclaringType == DeclaringTypeName && Rule.PropertyName == Property.GetFName())
		{
			++MatchCount;
			if (OutRule && !*OutRule)
			{
				*OutRule = &Rule;
			}
		}
	}
	return MatchCount;
}

TSharedRef<FJsonObject> FAssetDocumentPolicyRegistry::ExportAuthoredSurfaceRuleToJson(
	const FAssetDocumentAuthoredSurfacePropertyRule& Rule)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("PropertyPath"), Rule.GetPropertyPath());
	switch (Rule.Classification)
	{
	case EAssetDocumentAuthoredSurfaceClassification::ManagedAuthored:
		Json->SetStringField(TEXT("Classification"), TEXT("ManagedAuthored"));
		break;
	case EAssetDocumentAuthoredSurfaceClassification::ReferencedAssetOwned:
		Json->SetStringField(TEXT("Classification"), TEXT("ReferencedAssetOwned"));
		break;
	case EAssetDocumentAuthoredSurfaceClassification::DerivedCompilerDebugCacheTransient:
		Json->SetStringField(TEXT("Classification"), TEXT("DerivedCompilerDebugCacheTransient"));
		break;
	case EAssetDocumentAuthoredSurfaceClassification::InvalidLegacyObsolete:
		Json->SetStringField(TEXT("Classification"), TEXT("InvalidLegacyObsolete"));
		break;
	}
	if (!Rule.CanonicalPath.IsEmpty())
	{
		Json->SetStringField(TEXT("CanonicalPath"), Rule.CanonicalPath);
	}
	Json->SetStringField(TEXT("Reason"), Rule.Reason);
	Json->SetStringField(TEXT("SourceEvidence"), Rule.SourceEvidence);
	Json->SetStringField(TEXT("ExtractBehavior"), Rule.ExtractBehavior);
	Json->SetStringField(TEXT("DiffBehavior"), Rule.DiffBehavior);
	Json->SetStringField(TEXT("GuardTest"), Rule.GuardTest);
	return Json;
}
