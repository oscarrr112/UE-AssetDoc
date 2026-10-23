// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentPolicy.h"

#include "CoreMinimal.h"

class FJsonObject;
class FProperty;

enum class EAssetDocumentAuthoredSurfaceClassification : uint8
{
	ManagedAuthored,
	ReferencedAssetOwned,
	DerivedCompilerDebugCacheTransient,
	InvalidLegacyObsolete,
};

struct FAssetDocumentAuthoredSurfacePropertyRule
{
	FName DeclaringType;
	FName PropertyName;
	EAssetDocumentAuthoredSurfaceClassification Classification =
		EAssetDocumentAuthoredSurfaceClassification::DerivedCompilerDebugCacheTransient;
	FString CanonicalPath;
	FString Reason;
	FString SourceEvidence;
	FString ExtractBehavior;
	FString DiffBehavior;
	FString GuardTest;

	FString GetPropertyPath() const;
};

class FAssetDocumentPolicyRegistry
{
public:
	static bool GetBuiltinPreset(FName PresetName, FAssetDocumentRegionPolicyPreset& OutPreset);
	static TArray<FAssetDocumentRegionPolicyPreset> GetBuiltinPresets();
	static bool ExpandPreset(
		const FAssetDocumentRegionPolicyPreset& Preset,
		const FAssetDocumentRegionPolicyOverride& Override,
		FAssetDocumentRegionPolicy& OutPolicy);
	static bool MakeBuiltinRegionPolicy(
		FName PresetName,
		FName RegionId,
		EAssetDocumentRegionKind RegionKind,
		TArray<FString> ManagedUePropertyPaths,
		FAssetDocumentRegionPolicy& OutPolicy,
		FName CanonicalizerHookName = NAME_None);

	static TSharedRef<FJsonObject> ExportPolicyToJson(const FAssetDocumentRegionPolicy& Policy);
	static TSharedRef<FJsonObject> ExportPresetToJson(const FAssetDocumentRegionPolicyPreset& Preset);
	static int32 CountMatchingAuthoredSurfaceRules(
		const FProperty& Property,
		const TArray<FAssetDocumentAuthoredSurfacePropertyRule>& Rules,
		const FAssetDocumentAuthoredSurfacePropertyRule** OutRule = nullptr);
	static TSharedRef<FJsonObject> ExportAuthoredSurfaceRuleToJson(
		const FAssetDocumentAuthoredSurfacePropertyRule& Rule);
};
