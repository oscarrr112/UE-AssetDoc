// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentPythonRuntime.h"

#include "AssetDocumentPythonHost.h"
#include "AssetDocumentPythonNativeModule.h"
#include "AssetDocumentPythonRevisionRuntime.h"
#include "AssetDocumentRequestContextRuntime.h"
#include "IPythonScriptPlugin.h"

THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogAssetDocumentBTExpectedCleanup, Log, All);

namespace
{
FAssetDocumentRuntimeDiagnostic MakeRuntimeDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/PythonRuntime");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentContractValidationResult MakeRuntimeFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.Diagnostics.Add(MakeRuntimeDiagnostic(Code, Message));
	return Result;
}

class FAssetDocumentPythonRuntime final : public IAssetDocumentPythonRuntime
{
public:
	explicit FAssetDocumentPythonRuntime(
		const FAssetDocumentPythonRuntimeConfiguration& InConfiguration)
		: Configuration(InConfiguration)
		, Handles(MakeShared<FAssetDocumentHandleRegistryRuntime>())
		, Revisions(Handles)
		, Host(Revisions)
	{
	}

	~FAssetDocumentPythonRuntime() override
	{
		check(!bBindingInitialized);
	}

	FAssetDocumentContractValidationResult InitializePythonBinding() override
	{
		IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
		if (Python == nullptr || !Python->IsPythonInitialized())
		{
			return MakeRuntimeFailure(
				TEXT("AD.RUNTIME.PYTHON_NOT_READY"),
				TEXT("Engine Python is not initialized."));
		}
		PyGILState_STATE Gil = PyGILState_Ensure();
		FAssetDocumentContractValidationResult Result =
			AssetDocumentPythonInstallNativeModule();
		PyGILState_Release(Gil);
		bBindingInitialized = Result.bValid;
		return Result;
	}

	FAssetDocumentPythonRevisionActivationResult TryActivateRevision(
		const FAssetDocumentPythonRevisionCandidate& Candidate) override
	{
		FAssetDocumentPythonRevisionRuntimeCandidate PrivateCandidate;
		PrivateCandidate.Revision = Candidate.RuntimeRevision;
		PrivateCandidate.RepositoryRoot = Configuration.RepositoryRoot;
		PrivateCandidate.PackageRootRelativePath = Configuration.PackageRootRelativePath;
		PrivateCandidate.EntryModuleRelativePath = Candidate.EntryModuleRelativePath;
		PrivateCandidate.RegistryInput.RuntimeRevision = Candidate.RuntimeRevision;
		PrivateCandidate.RegistryInput.Required = Candidate.RequiredPrimitives;
		PrivateCandidate.RegistryInput.Providers = Candidate.Providers;
		PrivateCandidate.RegistryInput.AuthorityCatalog = Candidate.AuthorityCatalog;
		if (Candidate.DeriveRegistryInput)
		{
			PrivateCandidate.DeriveRegistryInput = [&Derive = Candidate.DeriveRegistryInput](
				TConstArrayView<FAssetDocumentPrimitiveRequirement> Declared,
				const FAssetDocumentRuntimeValueDto& Registrations,
				FAssetDocumentPrimitiveRegistryBuildInput& InOutInput,
				TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
			{
				TOptional<FAssetDocumentPythonDerivedRegistryInput> Derived =
					Derive(Declared, Registrations, OutDiagnostics);
				if (!Derived.IsSet())
				{
					return false;
				}
				InOutInput.Required = MoveTemp(Derived->RequiredPrimitives);
				InOutInput.Providers = MoveTemp(Derived->Providers);
				return true;
			};
		}
		const FAssetDocumentPythonRevisionRuntimeActivationResult PrivateResult =
			Revisions.TryActivate(PrivateCandidate);
		FAssetDocumentPythonRevisionActivationResult Result;
		Result.bActivated = PrivateResult.bActivated;
		Result.ActiveRuntimeRevision = Revisions.GetActiveRevision();
		Result.Diagnostics = PrivateResult.Diagnostics;
		return Result;
	}

	uint64 GetActiveRuntimeRevision() const override
	{
		return Revisions.GetActiveRevision();
	}

	FAssetDocumentRuntimeValueDto GetActiveRuntimeRegistrationProjection() const override
	{
		return Revisions.GetActiveRuntimeRegistrationProjection();
	}

	bool HasActiveRequests() const override
	{
		return Revisions.HasActiveRequests();
	}

	IAssetDocumentPythonRuntimeHost& GetHost() override
	{
		return Host;
	}

	const IAssetDocumentPrimitiveRegistry* GetActivePrimitiveRegistry() const override
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		TUniquePtr<FAssetDocumentPythonRevisionPin> Pin =
			Revisions.PinActiveRequest(Diagnostics);
		return Pin.IsValid() ? &Pin->GetRegistry().Get() : nullptr;
	}

	FAssetDocumentPythonRequestContextCreateResult CreateRequestContext(
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		FAssetDocumentPythonRequestContextCreateResult Result;
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		TUniquePtr<FAssetDocumentPythonRevisionPin> Pin =
			Revisions.PinActiveRequest(Diagnostics);
		if (!Pin.IsValid() || !Metadata.RequestId.IsValid()
			|| Metadata.PinnedRuntimeRevision != Pin->GetRevision())
		{
			Result.Diagnostics.Add(MakeRuntimeDiagnostic(
				TEXT("AD.RUNTIME.REVISION_INVALID"),
				TEXT("Request context metadata does not match the active revision.")));
			return Result;
		}
		Result.Context = MakeUnique<FAssetDocumentNativeRequestContextRuntime>(
			Metadata, TransactionId, Pin->GetRegistry(), Handles, ActiveTransaction);
		return Result;
	}

	FAssetDocumentHandleInvalidationResult InvalidateHandles(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		const auto Result = Handles->Invalidate(Event);
		IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
		if (!bBindingInitialized || !Python || !Python->IsPythonInitialized() || !Py_IsInitialized()
			|| (Event.Reason != EAssetDocumentHandleInvalidationReason::Commit
				&& Event.Reason != EAssetDocumentHandleInvalidationReason::Rollback)
			|| !Event.RequestId.IsValid() || Event.RuntimeRevision != Revisions.GetActiveRevision()) return Result;
		const PyGILState_STATE Gil = PyGILState_Ensure();
		PyObject *PreviousType = nullptr, *PreviousValue = nullptr, *PreviousTraceback = nullptr;
		PyErr_Fetch(&PreviousType, &PreviousValue, &PreviousTraceback);
		{
			TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
			auto Pin = Revisions.PinActiveRequest(Diagnostics);
			for (const auto& Diagnostic : Diagnostics)
				UE_LOG(LogAssetDocumentBTExpectedCleanup, Log, TEXT("%s: %s"), *Diagnostic.Code, *Diagnostic.Message);
			if (Pin && Pin->GetRevision() == Event.RuntimeRevision)
			{
				// The exact revision's existing dispatch may opt into BT pure-value cleanup.
				// No import, synthetic phase or product operation is executed here.
				PyObject* Callback = PyObject_GetAttrString(Pin->GetDispatchCallable(), "_clear_behavior_tree_expected_for_terminal");
				if (!Callback && PyErr_ExceptionMatches(PyExc_AttributeError)) PyErr_Clear();
				if (Callback)
				{
					const FString RequestId = Event.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower);
					PyObject* Called = PyObject_CallFunction(Callback, "sK", TCHAR_TO_UTF8(*RequestId),
						static_cast<unsigned long long>(Event.RuntimeRevision));
					Py_XDECREF(Called); Py_DECREF(Callback);
				}
			}
			if (PyErr_Occurred())
			{
				PyObject *Type = nullptr, *Value = nullptr, *Traceback = nullptr;
				PyErr_Fetch(&Type, &Value, &Traceback); PyErr_NormalizeException(&Type, &Value, &Traceback);
				PyObject* Message = Value ? PyObject_Str(Value) : nullptr;
				const char* Utf8 = Message ? PyUnicode_AsUTF8(Message) : nullptr;
				UE_LOG(LogAssetDocumentBTExpectedCleanup, Log, TEXT("AD.RUNTIME.BT_EXPECTED_CLEANUP_FAILED: %s"),
					Utf8 ? UTF8_TO_TCHAR(Utf8) : TEXT("Python cleanup exception"));
				Py_XDECREF(Message); Py_XDECREF(Type); Py_XDECREF(Value); Py_XDECREF(Traceback); PyErr_Clear();
			}
		}
		PyErr_Restore(PreviousType, PreviousValue, PreviousTraceback);
		PyGILState_Release(Gil);
		return Result;
	}

	void ShutdownPythonBinding() override
	{
		Revisions.Shutdown();
		if (!bBindingInitialized)
		{
			return;
		}
		PyGILState_STATE Gil = PyGILState_Ensure();
		AssetDocumentPythonRemoveNativeModule();
		PyGILState_Release(Gil);
		bBindingInitialized = false;
	}

private:
	FAssetDocumentPythonRuntimeConfiguration Configuration;
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Handles;
	mutable FAssetDocumentPythonRevisionRuntime Revisions;
	FAssetDocumentPythonHost Host;
	bool bBindingInitialized = false;
};
} // namespace

TUniquePtr<IAssetDocumentPythonRuntime> CreateAssetDocumentPythonRuntime(
	const FAssetDocumentPythonRuntimeConfiguration& Configuration)
{
	return MakeUnique<FAssetDocumentPythonRuntime>(Configuration);
}
