// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentPythonHost.h"

#include "AssetDocumentPythonDtoCodec.h"
#include "AssetDocumentPythonNativeModule.h"
#include "AssetDocumentPythonRevisionRuntime.h"

THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogAssetDocumentPythonHost, Log, All);

namespace
{
FAssetDocumentRuntimeDiagnostic MakeHostDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/PythonHost");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentRuntimeResultDto MakeHostFailure(
	const FAssetDocumentRuntimeRequestDto& Request,
	uint64 Revision,
	const FAssetDocumentRuntimeDiagnostic& Diagnostic)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	Result.RuntimeRevision = Revision;
	Result.Diagnostics.Add(Diagnostic);
	return Result;
}

void LogAndClearPythonException()
{
	PyObject* Type = nullptr;
	PyObject* Value = nullptr;
	PyObject* Traceback = nullptr;
	PyErr_Fetch(&Type, &Value, &Traceback);
	PyErr_NormalizeException(&Type, &Value, &Traceback);
	PyObject* Text = Value != nullptr ? PyObject_Str(Value) : nullptr;
	const char* Utf8 = Text != nullptr ? PyUnicode_AsUTF8(Text) : nullptr;
	UE_LOG(LogAssetDocumentPythonHost, Log, TEXT("AssetDocument Python dispatch failed: %s"),
		Utf8 != nullptr ? UTF8_TO_TCHAR(Utf8) : TEXT("<unavailable Python exception>"));
	Py_XDECREF(Text);
	Py_XDECREF(Traceback);
	Py_XDECREF(Value);
	Py_XDECREF(Type);
	PyErr_Clear();
}

struct FDispatchObjects final
{
	~FDispatchObjects()
	{
		if (Binding.IsValid())
		{
			AssetDocumentPythonCloseDispatchBinding(Binding.ToSharedRef());
		}
		Py_CLEAR(Result);
		Py_CLEAR(Request);
		Py_CLEAR(Context);
		Binding.Reset();
	}

	TSharedPtr<FAssetDocumentPythonDispatchBindingState> Binding;
	PyObject* Request = nullptr;
	PyObject* Context = nullptr;
	PyObject* Result = nullptr;
};

PyObject* CallPinnedDispatch(
	const FAssetDocumentRuntimeRequestDto& Request,
	IAssetDocumentNativeRequestContext& Context,
	const FAssetDocumentPythonRevisionPin& Pin,
	FDispatchObjects& Objects,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	Objects.Binding = AssetDocumentPythonOpenDispatchBinding(Context);
	Objects.Request = AssetDocumentPythonEncodeRequest(Request);
	Objects.Context = AssetDocumentPythonCreateRequestContextProxy(Objects.Binding.ToSharedRef());
	if (Objects.Request == nullptr) { return nullptr; }
	if (Objects.Context == nullptr) { return nullptr; }
	return Pin.CallDispatch(Objects.Request, Objects.Context, OutDiagnostic);
}

FAssetDocumentRuntimeResultDto MakeMissingDispatchFailure(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentPythonRevisionPin& Pin,
	FAssetDocumentRuntimeDiagnostic Diagnostic)
{
	if (PyErr_Occurred() != nullptr)
	{
		LogAndClearPythonException();
		Diagnostic = MakeHostDiagnostic(
			TEXT("AD.RUNTIME.PYTHON_EXCEPTION"),
			TEXT("Python dispatch raised an exception."));
	}
	if (Diagnostic.Code.IsEmpty())
	{
		UE_LOG(LogAssetDocumentPythonHost, Log,
			TEXT("AssetDocument Python dispatch failed without a retained Python exception."));
		Diagnostic = MakeHostDiagnostic(
			TEXT("AD.RUNTIME.PYTHON_EXCEPTION"),
			TEXT("Python dispatch raised an exception."));
	}
	return MakeHostFailure(Request, Pin.GetRevision(), Diagnostic);
}

FAssetDocumentRuntimeResultDto DecodeDispatchResult(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentPythonRevisionPin& Pin,
	PyObject* Object)
{
	FAssetDocumentRuntimeResultDto Result;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	if (!AssetDocumentPythonDecodeResult(Object, Result, Diagnostic))
	{
		PyErr_Clear();
		return MakeHostFailure(Request, Pin.GetRevision(), Diagnostic);
	}
	return Result;
}

FAssetDocumentRuntimeResultDto DispatchWithGil(
	const FAssetDocumentRuntimeRequestDto& Request,
	IAssetDocumentNativeRequestContext& Context,
	const FAssetDocumentPythonRevisionPin& Pin)
{
	FDispatchObjects Objects;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Objects.Result = CallPinnedDispatch(Request, Context, Pin, Objects, Diagnostic);
	if (Objects.Result == nullptr)
	{
		return MakeMissingDispatchFailure(Request, Pin, Diagnostic);
	}
	return DecodeDispatchResult(Request, Pin, Objects.Result);
}

FAssetDocumentRuntimeResultDto DispatchPinned(
	const FAssetDocumentRuntimeRequestDto& Request,
	IAssetDocumentNativeRequestContext& Context,
	const FAssetDocumentPythonRevisionPin& Pin)
{
	PyGILState_STATE Gil = PyGILState_Ensure();
	FAssetDocumentRuntimeResultDto Result =
		DispatchWithGil(Request, Context, Pin);
	PyGILState_Release(Gil);
	const FAssetDocumentContractValidationResult Validation =
		ValidateAssetDocumentRuntimeResult(Request, Result);
	if (!Validation.bValid)
	{
		return MakeHostFailure(Request, Pin.GetRevision(),
			MakeHostDiagnostic(TEXT("AD.RUNTIME.INVALID_RESULT_DTO"),
				TEXT("Python dispatch returned an invalid result DTO.")));
	}
	return Result;
}

bool IsDispatchContextValid(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRequestContextMetadata& Metadata,
	uint64 Revision)
{
	const bool bExpectedRevisionMatches =
		Request.ExpectedRuntimeRevision == 0
		|| Request.ExpectedRuntimeRevision == Revision;
	return bExpectedRevisionMatches
		&& Metadata.RequestId == Request.RequestId
		&& Metadata.PinnedRuntimeRevision == Revision;
}
} // namespace

FAssetDocumentPythonHost::FAssetDocumentPythonHost(
	FAssetDocumentPythonRevisionRuntime& InRevisions)
	: Revisions(InRevisions)
{
}

FAssetDocumentRuntimeResultDto FAssetDocumentPythonHost::Dispatch(
	const FAssetDocumentRuntimeRequestDto& Request,
	IAssetDocumentNativeRequestContext& Context)
{
	if (!IsInGameThread() || !ValidateAssetDocumentRuntimeRequest(Request).bValid)
	{
		return MakeHostFailure(Request, Revisions.GetActiveRevision(),
			MakeHostDiagnostic(TEXT("AD.RUNTIME.INVALID_REQUEST_DTO"),
				TEXT("Host dispatch requires a valid request on the game thread.")));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPythonRevisionPin> Pin =
		Revisions.PinActiveRequest(Diagnostics);
	if (!Pin.IsValid())
	{
		return MakeHostFailure(Request, Revisions.GetActiveRevision(), Diagnostics[0]);
	}
	if (!IsDispatchContextValid(
		Request, Context.GetMetadata(), Pin->GetRevision()))
	{
		return MakeHostFailure(Request, Pin->GetRevision(),
			MakeHostDiagnostic(TEXT("AD.RUNTIME.REVISION_INVALID"),
				TEXT("Request and native context do not match the pinned revision.")));
	}
	return DispatchPinned(Request, Context, *Pin);
}
