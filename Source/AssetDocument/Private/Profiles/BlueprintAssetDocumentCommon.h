// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentRegion.h"

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UClass;
class FProperty;
struct FBPVariableDescription;

enum class EBlueprintAssetDocumentNullArrayPolicy : uint8
{
	Reject,
	TreatAsOmitted
};

enum class EBlueprintAssetDocumentClassRefDiagnosticPolicy : uint8
{
	Common,
	WidgetBlueprint
};

struct FBlueprintAssetDocumentVariableSpec
{
	FName Name;
	FEdGraphPinType Type;
	FString DefaultValue;
	TOptional<FString> Category;
	TOptional<FString> Tooltip;
};

struct FBlueprintAssetDocumentInterfaceSpec
{
	UClass* InterfaceClass = nullptr;
};

/** Pure contracts shared by Blueprint-backed AssetDocument capabilities. */
class FBlueprintAssetDocumentCommon final
{
public:
	FBlueprintAssetDocumentCommon() = delete;

	static FAssetDocumentCapabilityResult Failure(
		const FString& Message,
		const FString& Path,
		const FString& Code);
	static bool ContainsBodyKey(const TArray<FName>& BodyKeys, const FString& BodyKey);
	static FAssetDocumentCapabilityResult RequireBodyObject(
		const TSharedRef<FJsonValue>& BodyJson,
		TSharedPtr<FJsonObject>& OutBody);

	static FString GetClassPath(const UClass* Class);
	static TSharedRef<FJsonObject> MakeClassRef(UClass* Class);
	static FAssetDocumentCapabilityResult ReadClassRef(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		UClass*& OutClass,
		EBlueprintAssetDocumentClassRefDiagnosticPolicy DiagnosticPolicy = EBlueprintAssetDocumentClassRefDiagnosticPolicy::Common);

	static FAssetDocumentCapabilityResult ReadPinType(
		const TSharedPtr<FJsonObject>& TypeObject,
		const FString& Path,
		FEdGraphPinType& OutPinType);
	static TSharedRef<FJsonObject> PinTypeToJsonObject(const FEdGraphPinType& PinType);
	static bool IsSupportedAuthoredPinType(const FEdGraphPinType& PinType);
	static bool AuthoredPinTypesDiffer(const FEdGraphPinType& Current, const FEdGraphPinType& Desired);
	static bool AuthoredDefaultValuesDiffer(
		const FEdGraphPinType& PinType,
		const FString& Current,
		const FString& Desired);

	static FAssetDocumentCapabilityResult ParseVariableSpecs(
		const TSharedPtr<FJsonObject>& BodyObject,
		TArray<FBlueprintAssetDocumentVariableSpec>& OutVariables,
		EBlueprintAssetDocumentNullArrayPolicy NullArrayPolicy = EBlueprintAssetDocumentNullArrayPolicy::Reject);
	static FBPVariableDescription* FindNewVariable(UBlueprint* Blueprint, FName Name);
	static const FBPVariableDescription* FindNewVariable(const UBlueprint* Blueprint, FName Name);
	static void ApplyVariableMetadata(UBlueprint* Blueprint, const FBlueprintAssetDocumentVariableSpec& Spec);
	static bool ParentClassHasPropertyNamed(const UClass* ParentClass, FName Name);
	static FAssetDocumentCapabilityResult ValidateVariablesAgainstParentClass(
		const UClass* ParentClass,
		const TArray<FBlueprintAssetDocumentVariableSpec>& Variables);
	static FString ResolveVariableDefaultValue(const UBlueprint* Blueprint, const FBPVariableDescription& Variable);
	static FString MakeVariableIdentityKey(FName Name);
	static TSharedRef<FJsonObject> VariableToJsonObject(
		const FBPVariableDescription& Variable,
		const UBlueprint* Blueprint = nullptr);
	static TSharedRef<FJsonObject> VariableSpecToJsonObject(const FBlueprintAssetDocumentVariableSpec& Variable);
	static TSharedPtr<FJsonValue> MakeVariableDiffValue(
		const UBlueprint* Blueprint,
		const FBPVariableDescription& Variable);

	static FAssetDocumentCapabilityResult ParseInterfaceSpecs(
		const TSharedPtr<FJsonObject>& BodyObject,
		TArray<FBlueprintAssetDocumentInterfaceSpec>& OutInterfaces,
		EBlueprintAssetDocumentNullArrayPolicy NullArrayPolicy = EBlueprintAssetDocumentNullArrayPolicy::Reject,
		EBlueprintAssetDocumentClassRefDiagnosticPolicy DiagnosticPolicy = EBlueprintAssetDocumentClassRefDiagnosticPolicy::Common);
	static TSharedRef<FJsonObject> InterfaceToJsonObject(UClass* InterfaceClass);
	static TSharedPtr<FJsonValue> MakeInterfaceDiffValue(UClass* InterfaceClass);

	static bool IsSupportedClassDefaultProperty(const FProperty* Property);
	static bool IsBlueprintVariableClassDefaultProperty(const UBlueprint* Blueprint, const FProperty* Property);
};

class FBlueprintAssetDocumentCommonRegionAdapter final : public IAssetDocumentRegionAdapter
{
public:
	explicit FBlueprintAssetDocumentCommonRegionAdapter(FName InAdapterName);

	virtual FName GetName() const override;
	virtual bool SupportsRegion(const FAssetDocumentRegionContext& Context) const override;
	virtual TSharedRef<FJsonObject> GetSchemaHint(const FAssetDocumentRegionContext& Context) const override;

	virtual FAssetDocumentCapabilityResult ValidateRegion(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue) const override;

	virtual FAssetDocumentCapabilityResult PreflightRegion(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue) const override;

	virtual FAssetDocumentCapabilityResult ApplyRegion(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		bool& bOutChanged) override;

	virtual FAssetDocumentCapabilityResult ExtractRegion(
		const FAssetDocumentRegionContext& Context,
		TSharedPtr<FJsonValue>& OutCurrentValue) const override;

	virtual FAssetDocumentCapabilityResult DiffRegion(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries) const override;

private:
	FName AdapterName;
};
