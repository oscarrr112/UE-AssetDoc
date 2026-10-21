// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentTypes.h"

struct FAssetDocumentRuntimeResultDto;

class ASSETDOCUMENT_API IAssetDocumentGenericRuntimeRoute
{
public:
	virtual ~IAssetDocumentGenericRuntimeRoute() = default;
	virtual FAssetDocumentResult DispatchApplyFile(
		const FAssetDocumentApplyFileRequest& Request) = 0;
	virtual FAssetDocumentResult DispatchValidate(
		const FAssetDocumentValidateRequest& Request) = 0;
	virtual FAssetDocumentRuntimeResultDto DispatchExtract(
		const FAssetDocumentExtractRequest& Request,
		const FString& AssetClassPath) = 0;
	virtual FAssetDocumentRuntimeResultDto DispatchDiff(
		const FAssetDocumentDiffRequest& Request) = 0;
	virtual FAssetDocumentRuntimeResultDto DispatchInspectProfile(
		const FAssetDocumentProfileRequest& Request,
		const FString& AssetClassPath) = 0;
	virtual FAssetDocumentRuntimeResultDto DispatchInspect(
		const FAssetDocumentInspectRequest& Request,
		const FString& AssetClassPath,
		const FString& AssetObjectPath) = 0;
	virtual FAssetDocumentRuntimeResultDto DispatchCreateTemplate(
		const FAssetDocumentTemplateRequest& Request,
		const FString& NormalizedTarget,
		const FString& AssetClassPath) = 0;
};
