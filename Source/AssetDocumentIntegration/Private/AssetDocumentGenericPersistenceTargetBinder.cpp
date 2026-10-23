// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGenericPersistenceTargetBinder.h"

#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

TOptional<FAssetDocumentCorePersistenceTargetBinding>
FAssetDocumentGenericPersistenceTargetBinder::Bind(
	const FAssetDocumentCorePersistenceTargetRequest& Request,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	UObject* Asset =
		FSoftObjectPath(FString(Request.CanonicalObjectPath)).ResolveObject();
	if (Asset == nullptr)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			OutDiagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/GenericRuntime/PersistenceTarget");
		Diagnostic.Code =
			TEXT("GenericRuntime.PersistenceTargetNotResolved");
		Diagnostic.Message =
			TEXT("The exact persistence target is not currently resolved.");
		return {};
	}
	FAssetDocumentCorePersistenceTargetBinding Binding;
	Binding.RequestId = Request.RequestId;
	Binding.TransactionId = Request.TransactionId;
	Binding.RuntimeRevision = Request.RuntimeRevision;
	Binding.Asset = Asset;
	Binding.Package = Asset->GetOutermost();
	return Binding;
}
