// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveRegistry.h"

THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

PyObject* AssetDocumentPythonEncodeRequest(const FAssetDocumentRuntimeRequestDto& Request);
bool AssetDocumentPythonDecodeResult(
	PyObject* Object,
	FAssetDocumentRuntimeResultDto& OutResult,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic);
PyObject* AssetDocumentPythonEncodeValue(
	const FAssetDocumentRuntimeValueDto& Value,
	int32 Depth = 0);
bool AssetDocumentPythonDecodeValue(
	PyObject* Object,
	FAssetDocumentRuntimeValueDto& OutValue,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic,
	int32 Depth = 0);
bool AssetDocumentPythonDecodeInvokeArguments(
	PyObject* ValueArguments,
	PyObject* HandleArguments,
	uint64 ContextBindingId,
	FAssetDocumentPrimitiveInvocation& OutInvocation,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic);
PyObject* AssetDocumentPythonEncodePrimitiveCatalog(
	const FAssetDocumentPrimitiveRegistrySnapshot& Snapshot,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic);
PyObject* AssetDocumentPythonEncodePrimitiveOutcome(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	uint64 ContextBindingId,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic);
