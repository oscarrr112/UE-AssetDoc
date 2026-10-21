// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentSidecarReader.h"

#include "AssetDocumentSidecar.h"

bool FAssetDocumentSidecarReader::LoadValidatedDocument(
	const FString& FilePath,
	TSharedPtr<FJsonObject>& OutDocument,
	FString& OutError)
{
	OutDocument.Reset();
	if (!FAssetDocumentSidecar::LoadJsonFile(FilePath, OutDocument, OutError))
	{
		OutDocument.Reset();
		return false;
	}

	if (!FAssetDocumentSidecar::ValidateTargetMatchesSidecar(FilePath, OutDocument, OutError))
	{
		OutDocument.Reset();
		return false;
	}

	return true;
}
