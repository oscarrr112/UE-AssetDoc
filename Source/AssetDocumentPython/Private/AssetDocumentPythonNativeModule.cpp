// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPythonNativeModule.h"

#include "AssetDocumentPythonDtoCodec.h"

THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

namespace
{
struct FRequestContextObject
{
	PyObject_HEAD
	uint64 BindingId = 0;
};

struct FOpaqueHandleObject
{
	PyObject_HEAD
	uint64 BindingId = 0;
	uint64 ProxyId = 0;
};

struct FOpaqueHandleRecord final
{
	uint64 BindingId = 0;
	FAssetDocumentOpaqueHandle Handle;
};

PyObject* NativeModule = nullptr;
// One per successful Install not yet matched by a Remove: every Python runtime (the production
// bootstrap and each test runtime) installs the one process-wide module, and only the last Remove
// tears it down. A test runtime shutting down must not take the types from the production runtime
// ("RequestContext type is unavailable" in every later request of the editor process).
int32 InstallCount = 0;
PyObject* RequestContextType = nullptr;
PyObject* OpaqueHandleType = nullptr;
uint64 NextBindingId = 1;
uint64 NextProxyId = 1;
TMap<uint64, TWeakPtr<FAssetDocumentPythonDispatchBindingState>> Bindings;
TMap<uint64, FOpaqueHandleRecord> OpaqueProxyRecords;

FAssetDocumentRuntimeDiagnostic MakeNativeDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/NativeModule");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentContractValidationResult MakeInstallResult(
	bool bValid,
	const TCHAR* Code = TEXT(""),
	const TCHAR* Message = TEXT(""))
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		Result.Diagnostics.Add(MakeNativeDiagnostic(Code, Message));
	}
	return Result;
}

PyObject* RaiseStableError(const TCHAR* Code, const TCHAR* Message)
{
	const FString Combined = FString(Code) + TEXT(": ") + Message;
	PyErr_SetString(PyExc_RuntimeError, TCHAR_TO_UTF8(*Combined));
	return nullptr;
}

TSharedPtr<FAssetDocumentPythonDispatchBindingState> PinBinding(uint64 BindingId)
{
	// Dispatch is game-thread-owned; releasing the GIL must not grant worker access.
	if (!IsInGameThread()) { return nullptr; }
	const TWeakPtr<FAssetDocumentPythonDispatchBindingState>* Weak =
		Bindings.Find(BindingId);
	if (Weak == nullptr)
	{
		return nullptr;
	}
	TSharedPtr<FAssetDocumentPythonDispatchBindingState> State = Weak->Pin();
	if (!State.IsValid() || !State->bOpen || State->Context == nullptr)
	{
		return nullptr;
	}
	return State;
}

PyObject* RejectConstruction(PyTypeObject* Type, PyObject* Args, PyObject* Keywords)
{
	(void)Type;
	(void)Args;
	(void)Keywords;
	PyErr_SetString(PyExc_TypeError, "native AssetDocument proxy types cannot be constructed");
	return nullptr;
}

PyObject* RejectReduce(PyObject* Self, PyObject* Args)
{
	(void)Self;
	(void)Args;
	PyErr_SetString(PyExc_TypeError, "native AssetDocument proxy types cannot be serialized");
	return nullptr;
}

void DeallocateRequestContext(PyObject* Self)
{
	PyObject_Del(Self);
}

void DeallocateOpaqueHandle(PyObject* Self)
{
	PyObject_Del(Self);
}

PyObject* OpaqueHandleRepr(PyObject* Self)
{
	(void)Self;
	return PyUnicode_FromString("<_assetdocument_native.OpaqueHandle redacted>");
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const FAssetDocumentPrimitiveRegistrySnapshot& Snapshot,
	FStringView PrimitiveId,
	uint32 Version)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Snapshot.Descriptors)
	{
		if (Descriptor.PrimitiveId == PrimitiveId && Descriptor.Version == Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

bool ContainsPrimitiveId(
	const FAssetDocumentPrimitiveRegistrySnapshot& Snapshot,
	FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Snapshot.Descriptors)
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return true;
		}
	}
	return false;
}

bool IsPrimitiveIdentityProjection(PyObject* PrimitiveObject, PyObject* VersionObject)
{
	return PyUnicode_CheckExact(PrimitiveObject) && PyLong_CheckExact(VersionObject);
}

bool IsDecodedPrimitiveIdentityValid(const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return !Invocation.PrimitiveId.IsEmpty() && Invocation.Version > 0;
}

bool DecodePrimitiveIdentity(
	PyObject* PrimitiveObject,
	PyObject* VersionObject,
	FAssetDocumentPrimitiveInvocation& OutInvocation)
{
	if (!IsPrimitiveIdentityProjection(PrimitiveObject, VersionObject))
	{
		return false;
	}
	const char* PrimitiveUtf8 = PyUnicode_AsUTF8(PrimitiveObject);
	const unsigned long Version = PyLong_AsUnsignedLong(VersionObject);
	if (PrimitiveUtf8 == nullptr || PyErr_Occurred() || Version > MAX_uint32)
	{
		PyErr_Clear();
		return false;
	}
	OutInvocation.PrimitiveId = UTF8_TO_TCHAR(PrimitiveUtf8);
	OutInvocation.Version = static_cast<uint32>(Version);
	return IsDecodedPrimitiveIdentityValid(OutInvocation);
}

bool DecodeNativeInvocation(PyObject* Args, uint64 BindingId,
	FAssetDocumentPrimitiveInvocation& OutInvocation, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	if (!PyTuple_CheckExact(Args) || PyTuple_Size(Args) != 4)
	{
		OutDiagnostic = MakeNativeDiagnostic(TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("invoke requires exactly four positional arguments."));
		return false;
	}
	if (!DecodePrimitiveIdentity(PyTuple_GetItem(Args, 0), PyTuple_GetItem(Args, 1), OutInvocation))
	{
		OutDiagnostic = MakeNativeDiagnostic(TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("Primitive identity must be a string and a nonzero uint32 version."));
		return false;
	}
	return AssetDocumentPythonDecodeInvokeArguments(PyTuple_GetItem(Args, 2), PyTuple_GetItem(Args, 3),
		BindingId, OutInvocation, OutDiagnostic);
}

bool DecodeHandoffAccess(
	PyObject* Object,
	EAssetDocumentHandleAccess& OutAccess)
{
	if (!PyUnicode_CheckExact(Object))
	{
		return false;
	}
	const char* Value = PyUnicode_AsUTF8(Object);
	if (Value == nullptr)
	{
		PyErr_Clear();
		return false;
	}
	if (FCStringAnsi::Strcmp(Value, "read_only") == 0)
	{
		OutAccess = EAssetDocumentHandleAccess::ReadOnly;
		return true;
	}
	if (FCStringAnsi::Strcmp(Value, "mutable") == 0)
	{
		OutAccess = EAssetDocumentHandleAccess::Mutable;
		return true;
	}
	return false;
}

PyObject* RequestContextHandoff(PyObject* Self, PyObject* Args)
{
	FRequestContextObject* ContextObject =
		reinterpret_cast<FRequestContextObject*>(Self);
	TSharedPtr<FAssetDocumentPythonDispatchBindingState> State =
		PinBinding(ContextObject->BindingId);
	if (!State.IsValid())
	{
		return RaiseStableError(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("The request-context binding is closed."));
	}
	if (!PyTuple_CheckExact(Args) || PyTuple_Size(Args) != 3)
	{
		return RaiseStableError(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("handoff requires exactly three positional arguments."));
	}

	FAssetDocumentOpaqueHandle SourceHandle;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	if (!AssetDocumentPythonResolveOpaqueHandleProxy(
			PyTuple_GetItem(Args, 0),
			ContextObject->BindingId,
			SourceHandle,
			Diagnostic))
	{
		return RaiseStableError(*Diagnostic.Code, *Diagnostic.Message);
	}
	PyObject* TargetProviderObject = PyTuple_GetItem(Args, 1);
	if (!PyUnicode_CheckExact(TargetProviderObject))
	{
		return RaiseStableError(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("The target provider must be an exact string."));
	}
	const char* TargetProviderUtf8 = PyUnicode_AsUTF8(TargetProviderObject);
	if (TargetProviderUtf8 == nullptr)
	{
		PyErr_Clear();
		return RaiseStableError(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("The target provider could not be decoded."));
	}
	EAssetDocumentHandleAccess RequestedAccess =
		EAssetDocumentHandleAccess::ReadOnly;
	if (!DecodeHandoffAccess(PyTuple_GetItem(Args, 2), RequestedAccess))
	{
		return RaiseStableError(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("The requested access must be read_only or mutable."));
	}

	const FAssetDocumentIssuedHandleResult Result = State->Context->Handoff(
		SourceHandle,
		FStringView(UTF8_TO_TCHAR(TargetProviderUtf8)),
		RequestedAccess);
	if (!Result.Handle.IsSet())
	{
		if (!Result.Diagnostics.IsEmpty())
		{
			return RaiseStableError(
				*Result.Diagnostics[0].Code,
				*Result.Diagnostics[0].Message);
		}
		return RaiseStableError(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("Handle handoff failed without a diagnostic."));
	}
	return AssetDocumentPythonCreateOpaqueHandleProxy(
		ContextObject->BindingId,
		Result.Handle.GetValue());
}

class FNativeInvocationGILRelease final
{
public:
 FNativeInvocationGILRelease() : SavedThread(PyEval_SaveThread()) {}
 ~FNativeInvocationGILRelease() { PyEval_RestoreThread(SavedThread); }
 FNativeInvocationGILRelease(const FNativeInvocationGILRelease&) = delete;
 FNativeInvocationGILRelease& operator=(const FNativeInvocationGILRelease&) = delete;
private:
 PyThreadState* SavedThread;
};

FAssetDocumentPrimitiveOutcome InvokeNativeWithoutGIL(IAssetDocumentNativeRequestContext& Context,
 const FAssetDocumentPrimitiveInvocation& Invocation)
{
 // Only copied native DTOs and the caller's pinned native state live here.
 // The public UE helper may wait on reference-scanner workers that need GIL.
 FNativeInvocationGILRelease Released;
 return Context.Invoke(Invocation);
}

PyObject* RequestContextInvoke(PyObject* Self, PyObject* Args)
{
	FRequestContextObject* ContextObject =
		reinterpret_cast<FRequestContextObject*>(Self);
	TSharedPtr<FAssetDocumentPythonDispatchBindingState> State =
		PinBinding(ContextObject->BindingId);
	if (!State.IsValid())
	{
		return RaiseStableError(TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("The request-context binding is closed."));
	}
	FAssetDocumentPrimitiveInvocation Invocation;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	if (!DecodeNativeInvocation(Args, ContextObject->BindingId, Invocation, Diagnostic))
	{
		return RaiseStableError(*Diagnostic.Code, *Diagnostic.Message);
	}
	const FAssetDocumentPrimitiveRegistrySnapshot Snapshot =
		State->Context->InspectCatalog();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(Snapshot, Invocation.PrimitiveId, Invocation.Version);
	if (Descriptor == nullptr)
	{
		if (ContainsPrimitiveId(Snapshot, Invocation.PrimitiveId))
		{
			const FAssetDocumentPrimitiveOutcome Outcome =
				InvokeNativeWithoutGIL(*State->Context, Invocation);
			check(!Outcome.Diagnostics.IsEmpty());
			return RaiseStableError(
				*Outcome.Diagnostics[0].Code,
				*Outcome.Diagnostics[0].Message);
		}
		return RaiseStableError(TEXT("AD.RUNTIME.PRIMITIVE_NOT_FOUND"),
			TEXT("The exact primitive identity is not in the frozen catalog."));
	}
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeNativeWithoutGIL(*State->Context, Invocation);
	return AssetDocumentPythonEncodePrimitiveOutcome(
		Outcome, *Descriptor, ContextObject->BindingId, Diagnostic);
}

PyObject* RequestContextInspectCatalog(PyObject* Self, PyObject* Args)
{
	(void)Args;
	FRequestContextObject* ContextObject =
		reinterpret_cast<FRequestContextObject*>(Self);
	TSharedPtr<FAssetDocumentPythonDispatchBindingState> State =
		PinBinding(ContextObject->BindingId);
	if (!State.IsValid())
	{
		return RaiseStableError(TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("The request-context binding is closed."));
	}
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	return AssetDocumentPythonEncodePrimitiveCatalog(
		State->Context->InspectCatalog(), Diagnostic);
}

TSharedPtr<FAssetDocumentPythonDispatchBindingState> PinContextObject(PyObject* Self)
{
	FRequestContextObject* ContextObject =
		reinterpret_cast<FRequestContextObject*>(Self);
	return PinBinding(ContextObject->BindingId);
}

PyObject* RequestContextRequestId(PyObject* Self, void* Closure)
{
	(void)Closure;
	TSharedPtr<FAssetDocumentPythonDispatchBindingState> State = PinContextObject(Self);
	if (!State.IsValid())
	{
		return RaiseStableError(TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("The request-context binding is closed."));
	}
	const FString RequestId = State->Context->GetMetadata().RequestId.ToString(
		EGuidFormats::DigitsWithHyphensLower);
	return PyUnicode_FromString(TCHAR_TO_UTF8(*RequestId));
}

PyObject* RequestContextRuntimeRevision(PyObject* Self, void* Closure)
{
	(void)Closure;
	TSharedPtr<FAssetDocumentPythonDispatchBindingState> State = PinContextObject(Self);
	if (!State.IsValid())
	{
		return RaiseStableError(TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("The request-context binding is closed."));
	}
	return PyLong_FromUnsignedLongLong(
		State->Context->GetMetadata().PinnedRuntimeRevision);
}

PyObject* RequestContextPhase(PyObject* Self, void* Closure)
{
	(void)Closure;
	TSharedPtr<FAssetDocumentPythonDispatchBindingState> State = PinContextObject(Self);
	if (!State.IsValid())
	{
		return RaiseStableError(TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("The request-context binding is closed."));
	}
	return PyLong_FromLong(static_cast<uint8>(State->Context->GetMetadata().Phase));
}

PyMethodDef RequestContextMethods[] = {
	{"handoff", RequestContextHandoff, METH_VARARGS, nullptr},
	{"invoke", RequestContextInvoke, METH_VARARGS, nullptr},
	{"inspect_catalog", RequestContextInspectCatalog, METH_NOARGS, nullptr},
	{"__reduce__", RejectReduce, METH_NOARGS, nullptr},
	{"__reduce_ex__", RejectReduce, METH_VARARGS, nullptr},
	{nullptr, nullptr, 0, nullptr},
};

PyGetSetDef RequestContextGetSet[] = {
	{"request_id", RequestContextRequestId, nullptr, nullptr, nullptr},
	{"runtime_revision", RequestContextRuntimeRevision, nullptr, nullptr, nullptr},
	{"phase", RequestContextPhase, nullptr, nullptr, nullptr},
	{nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyMethodDef OpaqueHandleMethods[] = {
	{"__reduce__", RejectReduce, METH_NOARGS, nullptr},
	{"__reduce_ex__", RejectReduce, METH_VARARGS, nullptr},
	{nullptr, nullptr, 0, nullptr},
};

PyType_Slot RequestContextSlots[] = {
	{Py_tp_new, reinterpret_cast<void*>(RejectConstruction)},
	{Py_tp_dealloc, reinterpret_cast<void*>(DeallocateRequestContext)},
	{Py_tp_methods, RequestContextMethods},
	{Py_tp_getset, RequestContextGetSet},
	{0, nullptr},
};

PyType_Slot OpaqueHandleSlots[] = {
	{Py_tp_new, reinterpret_cast<void*>(RejectConstruction)},
	{Py_tp_dealloc, reinterpret_cast<void*>(DeallocateOpaqueHandle)},
	{Py_tp_repr, reinterpret_cast<void*>(OpaqueHandleRepr)},
	{Py_tp_methods, OpaqueHandleMethods},
	{0, nullptr},
};

PyType_Spec RequestContextSpec = {
	"_assetdocument_native.RequestContext",
	sizeof(FRequestContextObject),
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE,
	RequestContextSlots,
};

PyType_Spec OpaqueHandleSpec = {
	"_assetdocument_native.OpaqueHandle",
	sizeof(FOpaqueHandleObject),
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE,
	OpaqueHandleSlots,
};

PyModuleDef NativeModuleDefinition = {
	PyModuleDef_HEAD_INIT,
	"_assetdocument_native",
	nullptr,
	-1,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
};

bool InstallTypes(PyObject* Module)
{
	RequestContextType = PyType_FromSpec(&RequestContextSpec);
	OpaqueHandleType = PyType_FromSpec(&OpaqueHandleSpec);
	if (RequestContextType == nullptr || OpaqueHandleType == nullptr)
	{
		return false;
	}
	PyObject* Dictionary = PyModule_GetDict(Module);
	return PyDict_SetItemString(Dictionary, "RequestContext", RequestContextType) == 0
		&& PyDict_SetItemString(Dictionary, "OpaqueHandle", OpaqueHandleType) == 0;
}

void RemoveBindingProxies(uint64 BindingId)
{
	for (auto Iterator = OpaqueProxyRecords.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator.Value().BindingId == BindingId)
		{
			Iterator.RemoveCurrent();
		}
	}
}

// Bindings whose state was released without AssetDocumentPythonCloseDispatchBinding (a runtime that
// released its install while the module stayed installed for another one) and their proxy records.
void PruneReleasedBindings()
{
	for (auto Iterator = Bindings.CreateIterator(); Iterator; ++Iterator)
	{
		if (!Iterator.Value().IsValid())
		{
			RemoveBindingProxies(Iterator.Key());
			Iterator.RemoveCurrent();
		}
	}
}

void DestroyNativeModule()
{
	for (TPair<uint64, TWeakPtr<FAssetDocumentPythonDispatchBindingState>>& Entry : Bindings)
	{
		if (TSharedPtr<FAssetDocumentPythonDispatchBindingState> State = Entry.Value.Pin())
		{
			State->Close();
		}
	}
	Bindings.Reset();
	OpaqueProxyRecords.Reset();
	if (PyObject* Modules = PyImport_GetModuleDict())
	{
		if (PyDict_DelItemString(Modules, "_assetdocument_native") != 0)
		{
			PyErr_Clear();
		}
	}
	Py_CLEAR(RequestContextType);
	Py_CLEAR(OpaqueHandleType);
	Py_CLEAR(NativeModule);
}
} // namespace

void FAssetDocumentPythonDispatchBindingState::Close()
{
	bOpen = false;
	Context = nullptr;
}

FAssetDocumentContractValidationResult AssetDocumentPythonInstallNativeModule()
{
	check(PyGILState_Check());
	if (NativeModule != nullptr)
	{
		++InstallCount;
		return MakeInstallResult(true);
	}
	NativeModule = PyModule_Create(&NativeModuleDefinition);
	if (NativeModule == nullptr || !InstallTypes(NativeModule))
	{
		PyErr_Clear();
		DestroyNativeModule();
		return MakeInstallResult(false,
			TEXT("AD.RUNTIME.NATIVE_MODULE_INSTALL_FAILED"),
			TEXT("The typed native module could not be installed."));
	}
	if (PyDict_SetItemString(
		PyImport_GetModuleDict(), "_assetdocument_native", NativeModule) != 0)
	{
		PyErr_Clear();
		DestroyNativeModule();
		return MakeInstallResult(false,
			TEXT("AD.RUNTIME.NATIVE_MODULE_INSTALL_FAILED"),
			TEXT("The typed native module could not be published."));
	}
	InstallCount = 1;
	return MakeInstallResult(true);
}

void AssetDocumentPythonRemoveNativeModule()
{
	check(PyGILState_Check());
	if (InstallCount > 1)
	{
		--InstallCount;
		return;
	}
	InstallCount = 0;
	DestroyNativeModule();
}

int32 AssetDocumentPythonNativeModuleInstallCount()
{
	return InstallCount;
}

TSharedRef<FAssetDocumentPythonDispatchBindingState>
AssetDocumentPythonOpenDispatchBinding(IAssetDocumentNativeRequestContext& Context)
{
	check(PyGILState_Check());
	check(NextBindingId != 0);
	PruneReleasedBindings();
	TSharedRef<FAssetDocumentPythonDispatchBindingState> State =
		MakeShared<FAssetDocumentPythonDispatchBindingState>();
	State->BindingId = NextBindingId++;
	State->Context = &Context;
	Bindings.Add(State->BindingId, State);
	return State;
}

void AssetDocumentPythonCloseDispatchBinding(
	const TSharedRef<FAssetDocumentPythonDispatchBindingState>& Binding)
{
	check(PyGILState_Check());
	Binding->Close();
	Bindings.Remove(Binding->BindingId);
	RemoveBindingProxies(Binding->BindingId);
}

bool IsOpaqueRecordBound(const FOpaqueHandleObject& Proxy, const FOpaqueHandleRecord* Record, uint64 BindingId)
{
	return Record != nullptr && Proxy.BindingId == BindingId && Record->BindingId == BindingId
		&& PinBinding(BindingId).IsValid();
}

PyObject* AssetDocumentPythonCreateRequestContextProxy(
	const TSharedRef<FAssetDocumentPythonDispatchBindingState>& Binding)
{
	check(PyGILState_Check());
	if (RequestContextType == nullptr || !Binding->bOpen)
	{
		return RaiseStableError(TEXT("AD.RUNTIME.NATIVE_MODULE_INSTALL_FAILED"),
			TEXT("RequestContext type is unavailable."));
	}
	FRequestContextObject* Object = PyObject_New(
		FRequestContextObject, reinterpret_cast<PyTypeObject*>(RequestContextType));
	Object->BindingId = Binding->BindingId;
	return reinterpret_cast<PyObject*>(Object);
}

PyObject* AssetDocumentPythonCreateOpaqueHandleProxy(
	uint64 ContextBindingId,
	const FAssetDocumentOpaqueHandle& Handle)
{
	check(PyGILState_Check());
	if (OpaqueHandleType == nullptr || !PinBinding(ContextBindingId).IsValid())
	{
		return RaiseStableError(TEXT("AD.RUNTIME.STALE_HANDLE"),
			TEXT("The opaque-handle binding is closed."));
	}
	check(NextProxyId != 0);
	FOpaqueHandleRecord Record;
	Record.BindingId = ContextBindingId;
	Record.Handle = Handle;
	const uint64 ProxyId = NextProxyId++;
	OpaqueProxyRecords.Add(ProxyId, MoveTemp(Record));
	FOpaqueHandleObject* Object = PyObject_New(
		FOpaqueHandleObject, reinterpret_cast<PyTypeObject*>(OpaqueHandleType));
	Object->BindingId = ContextBindingId;
	Object->ProxyId = ProxyId;
	return reinterpret_cast<PyObject*>(Object);
}

bool AssetDocumentPythonResolveOpaqueHandleProxy(
	PyObject* Object,
	uint64 ContextBindingId,
	FAssetDocumentOpaqueHandle& OutHandle,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	check(PyGILState_Check());
	if (!AssetDocumentPythonIsOpaqueHandleProxy(Object))
	{
		OutDiagnostic = MakeNativeDiagnostic(TEXT("AD.RUNTIME.STALE_HANDLE"),
			TEXT("The value is not an opaque-handle proxy."));
		return false;
	}
	FOpaqueHandleObject* Proxy = reinterpret_cast<FOpaqueHandleObject*>(Object);
	const FOpaqueHandleRecord* Record = OpaqueProxyRecords.Find(Proxy->ProxyId);
	if (!IsOpaqueRecordBound(*Proxy, Record, ContextBindingId))
	{
		OutDiagnostic = MakeNativeDiagnostic(TEXT("AD.RUNTIME.STALE_HANDLE"),
			TEXT("The opaque-handle proxy is stale or belongs to another binding."));
		return false;
	}
	OutHandle = Record->Handle;
	return true;
}

bool AssetDocumentPythonIsOpaqueHandleProxy(PyObject* Object)
{
	return Object != nullptr && OpaqueHandleType != nullptr
		&& Py_TYPE(Object) == reinterpret_cast<PyTypeObject*>(OpaqueHandleType);
}
