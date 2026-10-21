// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"

struct _object;
typedef _object PyObject;

struct FAssetDocumentPythonDispatchBindingState final
{
	uint64 BindingId = 0;
	IAssetDocumentNativeRequestContext* Context = nullptr;
	bool bOpen = true;

	void Close();
};

// Install/Remove are counted: the module (_assetdocument_native and its types) is created by the
// first Install and torn down by the matching last Remove, so several runtimes in one process
// (the production bootstrap and test runtimes) share it. Game thread, GIL held.
FAssetDocumentContractValidationResult AssetDocumentPythonInstallNativeModule();
void AssetDocumentPythonRemoveNativeModule();
// Installs not yet matched by a Remove (0: not installed).
int32 AssetDocumentPythonNativeModuleInstallCount();
TSharedRef<FAssetDocumentPythonDispatchBindingState> AssetDocumentPythonOpenDispatchBinding(
	IAssetDocumentNativeRequestContext& Context);
void AssetDocumentPythonCloseDispatchBinding(
	const TSharedRef<FAssetDocumentPythonDispatchBindingState>& Binding);
PyObject* AssetDocumentPythonCreateRequestContextProxy(
	const TSharedRef<FAssetDocumentPythonDispatchBindingState>& Binding);
PyObject* AssetDocumentPythonCreateOpaqueHandleProxy(
	uint64 ContextBindingId,
	const FAssetDocumentOpaqueHandle& Handle);
bool AssetDocumentPythonResolveOpaqueHandleProxy(
	PyObject* Object,
	uint64 ContextBindingId,
	FAssetDocumentOpaqueHandle& OutHandle,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic);
bool AssetDocumentPythonIsOpaqueHandleProxy(PyObject* Object);
