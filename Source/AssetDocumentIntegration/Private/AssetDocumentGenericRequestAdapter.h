// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentTypes.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class FAssetDocumentGenericRequestAdapter final
{
public:
	static TOptional<FAssetDocumentRuntimeRequestDto> AdaptApplyFile(
		const FAssetDocumentApplyFileRequest& Request,
		uint64 RuntimeRevision,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	static TOptional<FAssetDocumentRuntimeRequestDto> AdaptValidate(
		const FAssetDocumentValidateRequest& Request,
		uint64 RuntimeRevision,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	static TOptional<FAssetDocumentRuntimeRequestDto> AdaptExtract(
		const FAssetDocumentExtractRequest& Request,
		const FString& AssetClassPath,
		uint64 RuntimeRevision,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	static TOptional<FAssetDocumentRuntimeRequestDto> AdaptDiff(
		const FAssetDocumentDiffRequest& Request,
		uint64 RuntimeRevision,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	static TOptional<FAssetDocumentRuntimeRequestDto> AdaptInspectProfile(
		const FAssetDocumentProfileRequest& Request,
		const FString& AssetClassPath,
		uint64 RuntimeRevision,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	static TOptional<FAssetDocumentRuntimeRequestDto> AdaptInspect(
		const FAssetDocumentInspectRequest& Request,
		const FString& AssetClassPath,
		const FString& AssetObjectPath,
		uint64 RuntimeRevision,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	static TOptional<FAssetDocumentRuntimeRequestDto> AdaptCreateTemplate(
		const FAssetDocumentTemplateRequest& Request,
		const FString& NormalizedTarget,
		const FString& AssetClassPath,
		uint64 RuntimeRevision,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
};
