// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

struct FAssetDocumentUMGWidgetTreeQueryInput
{
	bool bIncludeProperties = false;
};

struct FAssetDocumentUMGAddSourceWidgetInput
{
	FName RequestedName;
	FString ClassPath;
	TOptional<FName> ParentName;
	TOptional<int32> ChildIndex;
};

struct FAssetDocumentUMGDeleteWidgetInput
{
	FString WarningPolicy;
};

struct FAssetDocumentUMGPanelChildAttachInput
{
	int32 RequestedIndex = 0;
};

struct FAssetDocumentUMGContentSetInput
{
	bool bClear = false;
};

struct FAssetDocumentUMGNamedSlotSetInput
{
	FName SlotName;
	bool bClear = false;
};

struct FAssetDocumentUMGDesignerBindingQueryInput
{
	TOptional<FName> WidgetName;
	TOptional<FName> PropertyName;
};

enum class EAssetDocumentUMGVariableNotifyOperation : uint8
{
	Add,
	Rename,
	Remove,
};

struct FAssetDocumentUMGVariableNotifyInput
{
	EAssetDocumentUMGVariableNotifyOperation Operation =
		EAssetDocumentUMGVariableNotifyOperation::Add;
	FName Name;
	FName OldName;
	FName NewName;
};

struct FAssetDocumentUMGAnimationQueryInput
{
	TOptional<FName> AnimationName;
};

struct FAssetDocumentUMGPossessableAddInput
{
	FString Name;
	FString ClassPath;
};

struct FAssetDocumentUMGPossessableRemoveInput
{
	FGuid PossessableGuid;
};

struct FAssetDocumentUMGSequenceBindingInput
{
	FGuid PossessableGuid;
	FName WidgetName;
};

struct FAssetDocumentUMGFloatKeyInput
{
	int32 Frame = 0;
	float Value = 0.0f;
	FString Interpolation;
};

struct FAssetDocumentUMGFloatChannelInput
{
	TOptional<float> DefaultValue;
	TArray<FAssetDocumentUMGFloatKeyInput> Keys;
};

struct FAssetDocumentUMGFloatTrackAddInput
{
	FGuid PossessableGuid;
	FName PropertyName;
	FString PropertyPath;
	int32 SectionStartFrame = 0;
	int32 SectionEndFrame = 0;
	FAssetDocumentUMGFloatChannelInput Channel;
};

struct FAssetDocumentUMGTransformTrackAddInput
{
	FGuid PossessableGuid;
	FName PropertyName;
	FString PropertyPath;
	int32 SectionStartFrame = 0;
	int32 SectionEndFrame = 0;
	TMap<FName, FAssetDocumentUMGFloatChannelInput> Channels;
};

enum class EAssetDocumentUMGTrackSectionRemoveTarget : uint8
{
	Track,
	Section,
};

struct FAssetDocumentUMGTrackSectionRemoveInput
{
	EAssetDocumentUMGTrackSectionRemoveTarget Target =
		EAssetDocumentUMGTrackSectionRemoveTarget::Track;
};

bool DecodeWidgetTreeQueryInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGWidgetTreeQueryInput& Out,
	FString& OutDiagnostic);
bool DecodeAddSourceWidgetInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGAddSourceWidgetInput& Out,
	FString& OutDiagnostic);
bool DecodeDeleteWidgetInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGDeleteWidgetInput& Out,
	FString& OutDiagnostic);
bool DecodePanelChildAttachInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGPanelChildAttachInput& Out,
	FString& OutDiagnostic);
bool DecodeContentSetInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGContentSetInput& Out,
	FString& OutDiagnostic);
bool DecodeNamedSlotSetInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGNamedSlotSetInput& Out,
	FString& OutDiagnostic);
bool DecodeDesignerBindingQueryInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGDesignerBindingQueryInput& Out,
	FString& OutDiagnostic);
bool DecodeVariableNotifyInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGVariableNotifyInput& Out,
	FString& OutDiagnostic);
bool DecodeAnimationQueryInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGAnimationQueryInput& Out,
	FString& OutDiagnostic);
bool DecodePossessableAddInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGPossessableAddInput& Out,
	FString& OutDiagnostic);
bool DecodePossessableRemoveInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGPossessableRemoveInput& Out,
	FString& OutDiagnostic);
bool DecodeSequenceBindInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGSequenceBindingInput& Out,
	FString& OutDiagnostic);
bool DecodeSequenceUnbindInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGSequenceBindingInput& Out,
	FString& OutDiagnostic);
bool DecodeFloatTrackAddInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGFloatTrackAddInput& Out,
	FString& OutDiagnostic);
bool DecodeTransformTrackAddInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGTransformTrackAddInput& Out,
	FString& OutDiagnostic);
bool DecodeTrackSectionRemoveInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGTrackSectionRemoveInput& Out,
	FString& OutDiagnostic);

ASSETDOCUMENTUMG_API FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGNull();
ASSETDOCUMENTUMG_API FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGBool(bool Value);
ASSETDOCUMENTUMG_API FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGInteger(int64 Value);
ASSETDOCUMENTUMG_API FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGNumber(double Value);
ASSETDOCUMENTUMG_API FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGString(FString Value);
ASSETDOCUMENTUMG_API FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGArray(
	TArray<FAssetDocumentRuntimeValueDto>&& Values);
ASSETDOCUMENTUMG_API FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGObject(
	std::initializer_list<TPair<FString, FAssetDocumentRuntimeValueDto>> Fields);
