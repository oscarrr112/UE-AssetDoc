// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

struct FMovieSceneFloatChannel;
class UClass;
class UMovieSceneSection;
class UMovieSceneTrack;
class UObject;
class UUserWidget;
class UWidget;
class UWidgetAnimation;
class UWidgetBlueprint;

ASSETDOCUMENTUMG_API TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentUMGUObjectPayload(UObject* Object);

ASSETDOCUMENTUMG_API TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentUMGChannelPayload(
	UMovieSceneSection* OwningSection,
	FMovieSceneFloatChannel* Channel,
	FName ChannelName);

UWidgetBlueprint* ResolveWidgetBlueprint(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

UClass* ResolveWidgetClass(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

UWidget* ResolveWidget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	bool bRequired = true);

UWidgetAnimation* ResolveWidgetAnimation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

UUserWidget* ResolvePreviewRoot(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

UMovieSceneTrack* ResolveMovieSceneTrack(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

UMovieSceneSection* ResolveMovieSceneSection(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	bool bRequired = true);

FMovieSceneFloatChannel* ResolveMovieSceneChannel(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	UMovieSceneSection*& OutOwningSection,
	FName& OutChannelName,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

bool IssueAssetDocumentUMGUObjectResult(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	const TCHAR* ResultName,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	UObject* Object,
	FAssetDocumentPrimitiveOutcome& InOutOutcome);

bool IssueAssetDocumentUMGChannelResult(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	const TCHAR* ResultName,
	EAssetDocumentHandleAccess Access,
	UMovieSceneSection* OwningSection,
	FMovieSceneFloatChannel* Channel,
	FName ChannelName,
	FAssetDocumentPrimitiveOutcome& InOutOutcome);
