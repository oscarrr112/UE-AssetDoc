// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentHandleRegistryRuntime.h"
#include "AssetDocumentPrimitiveRegistryRuntime.h"

struct _object;
typedef _object PyObject;

// Completes RegistryInput from the imported revision's declarations; false rejects it.
using FAssetDocumentPythonRegistryInputDerivation = TFunction<bool(
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Declared,
	const FAssetDocumentRuntimeValueDto& Registrations,
	FAssetDocumentPrimitiveRegistryBuildInput& InOutInput,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)>;

struct FAssetDocumentPythonRevisionRuntimeCandidate
{
	uint64 Revision = 0;
	FString RepositoryRoot;
	FString PackageRootRelativePath;
	FString EntryModuleRelativePath;
	FAssetDocumentPrimitiveRegistryBuildInput RegistryInput;
	// When bound, the entry must expose dispatch.describe_required_primitives().
	FAssetDocumentPythonRegistryInputDerivation DeriveRegistryInput;
};

struct FAssetDocumentPythonRevisionRuntimeActivationResult
{
	bool bActivated = false;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

struct FAssetDocumentPythonActiveRevision;
struct FAssetDocumentPythonRevisionState;

class FAssetDocumentPythonRevisionPin final
{
public:
	~FAssetDocumentPythonRevisionPin();
	uint64 GetRevision() const;
	PyObject* GetDispatchCallable() const;
	TSharedRef<FAssetDocumentPrimitiveRegistryRuntime> GetRegistry() const;
	PyObject* CallDispatch(PyObject* Request, PyObject* Context,
		FAssetDocumentRuntimeDiagnostic& OutDiagnostic) const;

private:
	friend class FAssetDocumentPythonRevisionRuntime;
	FAssetDocumentPythonRevisionPin(
		const TSharedRef<FAssetDocumentPythonRevisionState>& InState,
		const TSharedRef<FAssetDocumentPythonActiveRevision>& InRevision);
	TSharedRef<FAssetDocumentPythonRevisionState> State;
	TSharedRef<FAssetDocumentPythonActiveRevision> Revision;
};

class FAssetDocumentPythonRevisionRuntime final
{
public:
	explicit FAssetDocumentPythonRevisionRuntime(
		const TSharedRef<FAssetDocumentHandleRegistryRuntime>& InHandles);
	~FAssetDocumentPythonRevisionRuntime();
	FAssetDocumentPythonRevisionRuntimeActivationResult TryActivate(
		const FAssetDocumentPythonRevisionRuntimeCandidate& Candidate);
	TUniquePtr<FAssetDocumentPythonRevisionPin> PinActiveRequest(
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	uint64 GetActiveRevision() const;
	FAssetDocumentRuntimeValueDto GetActiveRuntimeRegistrationProjection() const;
	bool HasActiveRequests() const;
	void Shutdown();

private:
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Handles;
	TSharedRef<FAssetDocumentPythonRevisionState> State;
};
