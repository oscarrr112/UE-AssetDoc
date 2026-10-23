// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGenericReadOnlyDispatcher.h"

namespace
{
FAssetDocumentRuntimeResultDto MakeContextFailure(
	const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	if (Diagnostics.IsEmpty())
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/GenericRuntime/Validate");
		Diagnostic.Code = TEXT("GenericRuntime.ContextCreationFailed");
		Diagnostic.Message =
			TEXT("The Validate request context could not be created.");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
	}
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
	Result.Diagnostics = MoveTemp(Diagnostics);
	return Result;
}
}

FAssetDocumentRuntimeResultDto
FAssetDocumentGenericReadOnlyDispatcher::DispatchValidate(
	IAssetDocumentPythonRuntime& Runtime,
	const FAssetDocumentRuntimeRequestDto& Request)
{
	return DispatchPhase(
		Runtime,
		Request,
		EAssetDocumentRuntimeRequestPhase::Validate);
}

FAssetDocumentRuntimeResultDto
FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(
	IAssetDocumentPythonRuntime& Runtime,
	const FAssetDocumentRuntimeRequestDto& Request,
	EAssetDocumentRuntimeRequestPhase Phase)
{
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = Request.RequestId;
	Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision;
	Metadata.Phase = Phase;
	FAssetDocumentPythonRequestContextCreateResult Context =
		Runtime.CreateRequestContext(Metadata, FGuid::NewGuid(), nullptr);
	if (!Context.Context)
	{
		return MakeContextFailure(Request, MoveTemp(Context.Diagnostics));
	}
	return Runtime.GetHost().Dispatch(Request, *Context.Context);
}
