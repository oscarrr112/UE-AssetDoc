// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPythonRevisionRuntime.h"
#include "AssetDocumentPythonDtoCodec.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#include <limits.h>
#include <stdlib.h>

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
namespace
{
// Windows equivalent of realpath(): resolve symlinks/junctions and normalise
// case via the final path of an opened handle, then strip the \\?\ prefix.
bool ResolveFinalPathWindows(const FString& Absolute, FString& OutPath)
{
	const HANDLE File = CreateFileW(*Absolute, 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (File == INVALID_HANDLE_VALUE) { return false; }
	TArray<TCHAR> Buffer;
	Buffer.SetNumUninitialized(32768);
	const DWORD Length = GetFinalPathNameByHandleW(File, Buffer.GetData(), Buffer.Num(),
		FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle(File);
	if (Length == 0 || Length >= static_cast<DWORD>(Buffer.Num())) { return false; }
	FString Result(Length, Buffer.GetData());
	if (Result.StartsWith(TEXT("\\\\?\\UNC\\"))) { Result = TEXT("\\\\") + Result.RightChop(8); }
	else if (Result.StartsWith(TEXT("\\\\?\\"))) { Result.RightChopInline(4); }
	OutPath = MoveTemp(Result);
	return true;
}
}
#include "Windows/HideWindowsPlatformTypes.h"
#endif

THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

namespace
{
FAssetDocumentRuntimeDiagnostic MakeRevisionDiagnostic(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/PythonRevision"); Diagnostic.Code = Code; Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentPythonRevisionRuntimeActivationResult RejectRevision(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentPythonRevisionRuntimeActivationResult Result;
	Result.Diagnostics.Add(MakeRevisionDiagnostic(Code, Message));
	return Result;
}

bool IsSafeRelativePythonPath(const FString& Path)
{
	return !Path.IsEmpty() && FPaths::IsRelative(Path) && !Path.Contains(TEXT(".."))
		&& Path.EndsWith(TEXT(".py"));
}

bool CanonicalizeExisting(const FString& Path, FString& OutPath)
{
	const FString Absolute = FPaths::ConvertRelativePathToFull(Path);
#if PLATFORM_WINDOWS
	if (!ResolveFinalPathWindows(Absolute, OutPath)) { return false; }
#else
	char Resolved[PATH_MAX];
	if (realpath(TCHAR_TO_UTF8(*Absolute), Resolved) == nullptr) { return false; }
	OutPath = UTF8_TO_TCHAR(Resolved);
#endif
	FPaths::NormalizeFilename(OutPath); return true;
}

bool IsWithinDirectory(const FString& Path, const FString& Directory)
{
	return Path == Directory || Path.StartsWith(Directory + TEXT("/"));
}

bool IsCandidatePathShapeValid(const FAssetDocumentPythonRevisionRuntimeCandidate& Candidate)
{
	return IsSafeRelativePythonPath(Candidate.EntryModuleRelativePath)
		&& !Candidate.PackageRootRelativePath.IsEmpty() && FPaths::IsRelative(Candidate.PackageRootRelativePath)
		&& !Candidate.PackageRootRelativePath.Contains(TEXT(".."));
}

bool ResolveRequestedEntry(const FString& RequestedEntry, FString& OutEntry)
{
	if (IFileManager::Get().FileExists(*RequestedEntry)) { return CanonicalizeExisting(RequestedEntry, OutEntry); }
	OutEntry = RequestedEntry; FPaths::NormalizeFilename(OutEntry); FPaths::CollapseRelativeDirectories(OutEntry); return true;
}

bool ResolveRootAndPackage(const FAssetDocumentPythonRevisionRuntimeCandidate& Candidate, FString& OutRoot, FString& OutPackage)
{
	return CanonicalizeExisting(Candidate.RepositoryRoot, OutRoot)
		&& CanonicalizeExisting(FPaths::Combine(OutRoot, Candidate.PackageRootRelativePath), OutPackage);
}

bool BuildRepositoryPaths(const FAssetDocumentPythonRevisionRuntimeCandidate& Candidate, FString& OutEntry, FString& OutPackage)
{
	FString Repository; FString Package;
	if (!ResolveRootAndPackage(Candidate, Repository, Package)) { return false; }
	const FString RequestedEntry = FPaths::ConvertRelativePathToFull(FPaths::Combine(Package, Candidate.EntryModuleRelativePath));
	if (!ResolveRequestedEntry(RequestedEntry, OutEntry)) { return false; }
	if (!IsWithinDirectory(Package, Repository)) { return false; }
	if (!IsWithinDirectory(OutEntry, Package)) { return false; }
	OutPackage = MoveTemp(Package); return true;
}

bool AppendEnginePaths(TArray<FString>& OutAllowed)
{
	FString StandardLibrary; FString DynamicLibrary;
#if PLATFORM_WINDOWS
	// Engine Python 3.11 on Windows: <Win64>/Lib (stdlib), <Win64>/DLLs (extension modules).
	const FString PythonBase = FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries/ThirdParty/Python3/Win64"));
	const FString PythonRoot = FPaths::Combine(PythonBase, TEXT("Lib"));
	const FString DynamicRoot = FPaths::Combine(PythonBase, TEXT("DLLs"));
	const FString ZipRoot = PythonBase;
#else
	const FString PythonRoot = FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries/ThirdParty/Python3/Mac/lib/python3.11"));
	const FString DynamicRoot = FPaths::Combine(PythonRoot, TEXT("lib-dynload"));
	const FString ZipRoot = FPaths::GetPath(PythonRoot);
#endif
	if (!CanonicalizeExisting(PythonRoot, StandardLibrary)
		|| !CanonicalizeExisting(DynamicRoot, DynamicLibrary)) { return false; }
	OutAllowed.Add(StandardLibrary); OutAllowed.Add(DynamicLibrary);
	FString Zip;
	if (CanonicalizeExisting(FPaths::Combine(ZipRoot, TEXT("python311.zip")), Zip)) { OutAllowed.Add(Zip); }
	return true;
}

bool BuildAllowedPaths(const FAssetDocumentPythonRevisionRuntimeCandidate& Candidate,
	FString& OutEntry, TArray<FString>& OutAllowed)
{
	if (!IsCandidatePathShapeValid(Candidate)) { return false; }
	FString Package;
	if (!BuildRepositoryPaths(Candidate, OutEntry, Package)) { return false; }
	OutAllowed.Add(MoveTemp(Package)); return AppendEnginePaths(OutAllowed);
}

bool IsAllowedOrigin(const FString& Origin, TConstArrayView<FString> Allowed)
{
	FString Canonical;
	if (!CanonicalizeExisting(Origin, Canonical)) { return false; }
	for (const FString& Root : Allowed)
	{
		if (IsWithinDirectory(Canonical, Root)) { return true; }
	}
	return false;
}

bool IsFileBackedModuleAllowed(PyObject* Module, TConstArrayView<FString> Allowed)
{
	PyObject* File = PyObject_GetAttrString(Module, "__file__");
	if (File == nullptr) { PyErr_Clear(); return true; }
	const char* Utf8 = PyUnicode_Check(File) ? PyUnicode_AsUTF8(File) : nullptr;
	const bool bAllowed = Utf8 != nullptr && IsAllowedOrigin(UTF8_TO_TCHAR(Utf8), Allowed);
	Py_DECREF(File); return bAllowed;
}

class FRevisionImportScope final
{
public:
	explicit FRevisionImportScope(TConstArrayView<FString> InAllowed)
		: Allowed(InAllowed)
	{
		PyObject* Sys = PyImport_ImportModule("sys");
		Path = Sys != nullptr ? PyObject_GetAttrString(Sys, "path") : nullptr;
		OriginalPath = Path != nullptr ? PySequence_List(Path) : nullptr;
		Modules = PyImport_GetModuleDict(); OriginalModules = Modules != nullptr ? PyDict_Copy(Modules) : nullptr;
		Py_XDECREF(Sys); bValid = ReplacePath() && RemoveDisallowedModules();
	}
	~FRevisionImportScope() { Restore(); }
	bool IsValid() const { return bValid; }
	bool AreLoadedOriginsAllowed() const
	{
		PyObject* Values = PyDict_Values(Modules);
		bool bAllowed = Values != nullptr;
		for (Py_ssize_t Index = 0; bAllowed && Index < PyList_Size(Values); ++Index)
		{
			bAllowed = IsFileBackedModuleAllowed(PyList_GetItem(Values, Index), Allowed);
		}
		Py_XDECREF(Values); return bAllowed;
	}
private:
	bool HasValidSnapshot() const
	{
		return Path != nullptr && OriginalPath != nullptr && OriginalModules != nullptr && PyList_CheckExact(Path);
	}
	bool AppendAllowedPaths()
	{
		for (const FString& Root : Allowed)
		{
			PyObject* Item = PyUnicode_FromString(TCHAR_TO_UTF8(*Root));
			const int Result = Item != nullptr ? PyList_Append(Path, Item) : -1; Py_XDECREF(Item);
			if (Result != 0) { return false; }
		}
		return true;
	}
	bool ReplacePath()
	{
		if (!HasValidSnapshot()) { return false; }
		if (PyList_SetSlice(Path, 0, PyList_Size(Path), nullptr) != 0) { return false; }
		return AppendAllowedPaths();
	}
	bool RemoveDisallowedModules()
	{
		PyObject* Keys = PyDict_Keys(Modules);
		if (Keys == nullptr) { return false; }
		for (Py_ssize_t Index = 0; Index < PyList_Size(Keys); ++Index)
		{
			PyObject* Key = PyList_GetItem(Keys, Index); PyObject* Module = PyDict_GetItem(Modules, Key);
			if (Module != nullptr && !IsFileBackedModuleAllowed(Module, Allowed)) { PyDict_DelItem(Modules, Key); }
		}
		Py_DECREF(Keys); return true;
	}
	void Restore()
	{
		if (Modules != nullptr && OriginalModules != nullptr) { PyDict_Clear(Modules); PyDict_Update(Modules, OriginalModules); }
		if (Path != nullptr && OriginalPath != nullptr) { PyList_SetSlice(Path, 0, PyList_Size(Path), OriginalPath); }
		Py_XDECREF(OriginalModules); Py_XDECREF(OriginalPath); Py_XDECREF(Path); PyErr_Clear();
	}
	TArray<FString> Allowed; PyObject* Path = nullptr; PyObject* OriginalPath = nullptr;
	PyObject* Modules = nullptr; PyObject* OriginalModules = nullptr; bool bValid = false;
};

// Validate the frozen projection wire only. Editor lifecycle/catalog admission remains
// the later activation owner's responsibility.
const FAssetDocumentRuntimeValueDto* RegistrationField(
	const FAssetDocumentRuntimeValueDto& Object, const TCHAR* Key)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object) { return nullptr; }
	const int32 Index = Object.ObjectKeys.IndexOfByKey(Key);
	return Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

bool IsRegistrationString(const FAssetDocumentRuntimeValueDto* Value)
{
	return Value != nullptr && Value->Kind == EAssetDocumentRuntimeValueKind::String
		&& !Value->StringValue.IsEmpty();
}

bool AreRegistrationBodySectionsValid(const FAssetDocumentRuntimeValueDto* Sections)
{
	if (Sections == nullptr || Sections->Kind != EAssetDocumentRuntimeValueKind::Array) { return false; }
	TSet<FString> Names;
	for (const FAssetDocumentRuntimeValueDto& Section : Sections->Elements)
	{
		if (!IsRegistrationString(&Section) || Names.Contains(Section.StringValue)) { return false; }
		Names.Add(Section.StringValue);
	}
	return true;
}

bool AreRegistrationRequirementsValid(const FAssetDocumentRuntimeValueDto* Requirements)
{
	if (Requirements == nullptr || Requirements->Kind != EAssetDocumentRuntimeValueKind::Array) { return false; }
	TMap<FString, TSet<FString>> RoutesByPrimitive;
	for (const FAssetDocumentRuntimeValueDto& Requirement : Requirements->Elements)
	{
		const auto* Id = RegistrationField(Requirement, TEXT("PrimitiveId"));
		const auto* Version = RegistrationField(Requirement, TEXT("Version"));
		const auto* Route = RegistrationField(Requirement, TEXT("RequiredByRouteId"));
		if (Requirement.ObjectKeys.Num() != 3 || !IsRegistrationString(Id) || !IsRegistrationString(Route)
			|| Version == nullptr || Version->Kind != EAssetDocumentRuntimeValueKind::Integer
			|| Version->IntegerValue <= 0 || Version->IntegerValue > MAX_uint32) { return false; }
		TSet<FString>& Routes = RoutesByPrimitive.FindOrAdd(Id->StringValue);
		if (Routes.Contains(Route->StringValue)) { return false; }
		Routes.Add(Route->StringValue);
	}
	return true;
}

bool IsRegistrationProjectionValid(const FAssetDocumentRuntimeValueDto& Projection)
{
	if (Projection.Kind != EAssetDocumentRuntimeValueKind::Array) { return false; }
	TSet<FString> Classes;
	for (const FAssetDocumentRuntimeValueDto& Row : Projection.Elements)
	{
		const auto* Class = RegistrationField(Row, TEXT("Class"));
		const auto* Registration = RegistrationField(Row, TEXT("RuntimeRegistration"));
		if (Row.ObjectKeys.Num() != 3 || !IsRegistrationString(Class) || Classes.Contains(Class->StringValue)
			|| !AreRegistrationBodySectionsValid(RegistrationField(Row, TEXT("BodySections")))
			|| Registration == nullptr || Registration->Kind != EAssetDocumentRuntimeValueKind::Object
			|| (Registration->ObjectKeys.Num() != 2 && Registration->ObjectKeys.Num() != 3)) { return false; }
		const auto* Policy = RegistrationField(*Registration, TEXT("TargetPolicy"));
		const auto* Lifecycle = RegistrationField(*Registration, TEXT("LifecycleId"));
		const bool bExistingOnly = Registration->ObjectKeys.Num() == 2 && Lifecycle == nullptr
			&& IsRegistrationString(Policy) && Policy->StringValue == TEXT("UpdateOnly");
		const bool bCreationLifecycle = Registration->ObjectKeys.Num() == 3 && IsRegistrationString(Lifecycle)
			&& IsRegistrationString(Policy)
			&& (Policy->StringValue == TEXT("CreateOrUpdate") || Policy->StringValue == TEXT("UpdateOnly"));
		if ((!bExistingOnly && !bCreationLifecycle)
			|| !AreRegistrationRequirementsValid(RegistrationField(*Registration, TEXT("RequiredPrimitives")))) { return false; }
		Classes.Add(Class->StringValue);
	}
	return true;
}

bool HasDeclaredRegistrationProjectionMember(PyObject* Dispatch)
{
	// AttributeError from a declared descriptor is a failed projection lookup,
	// not the backwards-compatible absence of the optional member.
	PyObject* Mro = Py_TYPE(Dispatch)->tp_mro;
	if (Mro == nullptr) { return false; }
	for (Py_ssize_t Index = 0; Index < PyTuple_Size(Mro); ++Index)
	{
		PyTypeObject* Type = reinterpret_cast<PyTypeObject*>(PyTuple_GetItem(Mro, Index));
		if (Type->tp_dict != nullptr && PyDict_GetItemString(Type->tp_dict, "describe_runtime_registrations") != nullptr) { return true; }
	}
	return false;
}

bool CaptureRegistrationProjection(PyObject* Dispatch,
	FAssetDocumentRuntimeValueDto& OutProjection, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	OutProjection.Kind = EAssetDocumentRuntimeValueKind::Array;
	PyObject* Describe = PyObject_GetAttrString(Dispatch, "describe_runtime_registrations");
	if (Describe == nullptr)
	{
		if (PyErr_ExceptionMatches(PyExc_AttributeError) && !HasDeclaredRegistrationProjectionMember(Dispatch)) { PyErr_Clear(); return true; }
		PyErr_Clear();
		OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_CALL_FAILED"), TEXT("Registration projection member lookup failed."));
		return false;
	}
	if (!PyCallable_Check(Describe))
	{
		Py_DECREF(Describe);
		OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID"), TEXT("Registration projection member must be callable."));
		return false;
	}
	PyObject* Projection = PyObject_CallNoArgs(Describe);
	Py_DECREF(Describe);
	if (Projection == nullptr)
	{
		PyErr_Clear();
		OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_CALL_FAILED"), TEXT("Registration projection call failed."));
		return false;
	}
	const bool bValid = AssetDocumentPythonDecodeValue(Projection, OutProjection, OutDiagnostic)
		&& IsRegistrationProjectionValid(OutProjection);
	Py_DECREF(Projection); PyErr_Clear();
	if (!bValid)
	{
		OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID"), TEXT("Registration projection violates the frozen wire contract."));
	}
	return bValid;
}

// Wire: tuple of {PrimitiveId, Version, RequiredByRouteId} with unique exact triples.
bool ParseDeclaredRequirements(const FAssetDocumentRuntimeValueDto& Wire,
	TArray<FAssetDocumentPrimitiveRequirement>& OutRequirements)
{
	if (Wire.Kind != EAssetDocumentRuntimeValueKind::Array) { return false; }
	for (const FAssetDocumentRuntimeValueDto& Row : Wire.Elements)
	{
		const auto* Id = RegistrationField(Row, TEXT("PrimitiveId"));
		const auto* Version = RegistrationField(Row, TEXT("Version"));
		const auto* Route = RegistrationField(Row, TEXT("RequiredByRouteId"));
		if (Row.ObjectKeys.Num() != 3 || !IsRegistrationString(Id) || !IsRegistrationString(Route)
			|| Version == nullptr || Version->Kind != EAssetDocumentRuntimeValueKind::Integer
			|| Version->IntegerValue <= 0 || Version->IntegerValue > MAX_uint32) { return false; }
		FAssetDocumentPrimitiveRequirement Requirement{Id->StringValue, static_cast<uint32>(Version->IntegerValue), Route->StringValue};
		if (OutRequirements.ContainsByPredicate([&](const FAssetDocumentPrimitiveRequirement& Existing)
			{
				return Existing.PrimitiveId == Requirement.PrimitiveId && Existing.Version == Requirement.Version
					&& Existing.RequiredByRouteId == Requirement.RequiredByRouteId;
			})) { return false; }
		OutRequirements.Add(MoveTemp(Requirement));
	}
	return true;
}

bool CaptureDeclaredRequirements(PyObject* Dispatch,
	TArray<FAssetDocumentPrimitiveRequirement>& OutRequirements, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	PyObject* Describe = PyObject_GetAttrString(Dispatch, "describe_required_primitives");
	if (Describe == nullptr || !PyCallable_Check(Describe))
	{
		Py_XDECREF(Describe); PyErr_Clear();
		OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_INVALID"), TEXT("Entry dispatch must expose a callable describe_required_primitives()."));
		return false;
	}
	PyObject* Wire = PyObject_CallNoArgs(Describe);
	Py_DECREF(Describe);
	if (Wire == nullptr)
	{
		PyErr_Clear();
		OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_CALL_FAILED"), TEXT("describe_required_primitives() raised."));
		return false;
	}
	FAssetDocumentRuntimeValueDto Value;
	const bool bValid = AssetDocumentPythonDecodeValue(Wire, Value, OutDiagnostic)
		&& ParseDeclaredRequirements(Value, OutRequirements);
	Py_DECREF(Wire); PyErr_Clear();
	if (!bValid)
	{
		OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_INVALID"), TEXT("Declared required primitives violate the wire contract."));
	}
	return bValid;
}

struct FImportedCandidate
{
	PyObject* Module = nullptr; PyObject* Dispatch = nullptr;
	FAssetDocumentRuntimeValueDto RegistrationProjection;
	TArray<FAssetDocumentPrimitiveRequirement> DeclaredRequirements;
	~FImportedCandidate() { Py_XDECREF(Dispatch); Py_XDECREF(Module); }
};

struct FImportObjects final
{
	~FImportObjects()
	{
		Py_XDECREF(Dispatch); Py_XDECREF(Exec); Py_XDECREF(Loader); Py_XDECREF(Module); Py_XDECREF(Spec);
		Py_XDECREF(PyEntry); Py_XDECREF(PyName); Py_XDECREF(ModuleFunction); Py_XDECREF(SpecFunction); Py_XDECREF(Util);
	}
	bool LoadFunctions()
	{
		Util = PyImport_ImportModule("importlib.util");
		if (Util == nullptr) { return false; }
		SpecFunction = PyObject_GetAttrString(Util, "spec_from_file_location");
		if (SpecFunction == nullptr) { return false; }
		ModuleFunction = PyObject_GetAttrString(Util, "module_from_spec"); return ModuleFunction != nullptr;
	}
	bool CreateSpec(uint64 Revision, const FString& Entry)
	{
		Name = FString::Printf(TEXT("_assetdocument_revision_%llu"), Revision);
		PyName = PyUnicode_FromString(TCHAR_TO_UTF8(*Name)); PyEntry = PyUnicode_FromString(TCHAR_TO_UTF8(*Entry));
		if (PyName == nullptr) { return false; }
		if (PyEntry == nullptr) { return false; }
		Spec = PyObject_CallFunctionObjArgs(SpecFunction, PyName, PyEntry, nullptr); return Spec != nullptr;
	}
	bool CreateModule()
	{
		Module = PyObject_CallOneArg(ModuleFunction, Spec);
		if (Module == nullptr) { return false; }
		Loader = PyObject_GetAttrString(Spec, "loader");
		if (Loader == nullptr) { return false; }
		Exec = PyObject_GetAttrString(Loader, "exec_module"); return Exec != nullptr;
	}
	bool Execute()
	{
		if (!PyCallable_Check(Exec)) { return false; }
		if (PyDict_SetItemString(PyImport_GetModuleDict(), TCHAR_TO_UTF8(*Name), Module) != 0) { return false; }
		PyObject* Called = PyObject_CallOneArg(Exec, Module);
		if (Called == nullptr) { return false; }
		Py_DECREF(Called); return true;
	}
	bool CaptureDispatch(bool& bDispatchMissing)
	{
		Dispatch = PyObject_GetAttrString(Module, "dispatch");
		bDispatchMissing = Dispatch == nullptr || !PyCallable_Check(Dispatch); return !bDispatchMissing;
	}
	FString Name; PyObject* Util = nullptr; PyObject* SpecFunction = nullptr; PyObject* ModuleFunction = nullptr;
	PyObject* PyName = nullptr; PyObject* PyEntry = nullptr; PyObject* Spec = nullptr; PyObject* Module = nullptr;
	PyObject* Loader = nullptr; PyObject* Exec = nullptr; PyObject* Dispatch = nullptr;
};

bool PrepareImportObjects(FImportObjects& Objects, uint64 Revision, const FString& Entry)
{
	if (!Objects.LoadFunctions()) { return false; }
	if (!Objects.CreateSpec(Revision, Entry)) { return false; }
	if (!Objects.CreateModule()) { return false; }
	return Objects.Execute();
}

bool ImportCandidate(uint64 Revision, const FString& Entry, TConstArrayView<FString> Allowed, bool bCaptureDeclared,
	FImportedCandidate& OutCandidate, bool& bDispatchMissing, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	FRevisionImportScope Scope(Allowed);
	if (!Scope.IsValid()) { return false; }
	FImportObjects Objects;
	if (!PrepareImportObjects(Objects, Revision, Entry))
	{
		PyErr_Clear(); return false;
	}
	if (!Objects.CaptureDispatch(bDispatchMissing)) { PyErr_Clear(); return false; }
	if (!CaptureRegistrationProjection(Objects.Dispatch, OutCandidate.RegistrationProjection, OutDiagnostic)) { return false; }
	if (bCaptureDeclared && !CaptureDeclaredRequirements(Objects.Dispatch, OutCandidate.DeclaredRequirements, OutDiagnostic)) { return false; }
	if (!Scope.AreLoadedOriginsAllowed()) { PyErr_Clear(); return false; }
	OutCandidate.Module = Py_NewRef(Objects.Module); OutCandidate.Dispatch = Py_NewRef(Objects.Dispatch); return true;
}
} // namespace

struct FAssetDocumentPythonActiveRevision final
{
	~FAssetDocumentPythonActiveRevision()
	{
		PyGILState_STATE Gil = PyGILState_Ensure(); Py_XDECREF(Dispatch); Py_XDECREF(Module); PyGILState_Release(Gil);
	}
	uint64 Revision = 0; PyObject* Module = nullptr; PyObject* Dispatch = nullptr;
	FAssetDocumentRuntimeValueDto RegistrationProjection;
	TSharedPtr<FAssetDocumentPrimitiveRegistryRuntime> Registry; TArray<FString> AllowedPaths;
};

struct FAssetDocumentPythonRevisionState final
{
	mutable FCriticalSection Mutex; TSharedPtr<FAssetDocumentPythonActiveRevision> Active;
	int32 ActiveRequests = 0; bool bShutdown = false;
};

bool IsCandidateIdentityValid(const FAssetDocumentPythonRevisionRuntimeCandidate& Candidate)
{
	return Candidate.Revision > 0 && Candidate.RegistryInput.RuntimeRevision == Candidate.Revision;
}

FAssetDocumentPythonRevisionRuntimeActivationResult CheckActivationAllowed(
	const TSharedRef<FAssetDocumentPythonRevisionState>& State)
{
	FScopeLock Lock(&State->Mutex);
	if (State->bShutdown) { return RejectRevision(TEXT("AD.RUNTIME.REVISION_INVALID"), TEXT("Revision runtime is shut down.")); }
	if (State->ActiveRequests > 0) { return RejectRevision(TEXT("AD.RUNTIME.REVISION_ACTIVE_REQUEST"), TEXT("An active request pins the current revision.")); }
	FAssetDocumentPythonRevisionRuntimeActivationResult Result; Result.bActivated = true; return Result;
}

struct FRevisionPreparation final
{
	TSharedPtr<FAssetDocumentPythonActiveRevision> Replacement;
	FAssetDocumentPythonRevisionRuntimeActivationResult Failure;
};

namespace
{
const TCHAR* SelectImportFailureCode(bool bDispatchMissing)
{
	return bDispatchMissing
		? TEXT("AD.RUNTIME.REVISION_DISPATCH_MISSING")
		: TEXT("AD.RUNTIME.REVISION_IMPORT_FAILED");
}
} // namespace

FRevisionPreparation PrepareRevision(const FAssetDocumentPythonRevisionRuntimeCandidate& Candidate)
{
	FRevisionPreparation Prepared; FString Entry; TArray<FString> Allowed;
	if (!IsCandidateIdentityValid(Candidate) || !BuildAllowedPaths(Candidate, Entry, Allowed))
	{
		Prepared.Failure = RejectRevision(TEXT("AD.RUNTIME.REVISION_INVALID"), TEXT("Candidate paths or revision identity are invalid.")); return Prepared;
	}
	// Import before freezing: the registry input may be derived from the revision's declarations.
	PyGILState_STATE Gil = PyGILState_Ensure(); FImportedCandidate Imported; bool bDispatchMissing = false;
	FAssetDocumentRuntimeDiagnostic ImportDiagnostic;
	const bool bImported = ImportCandidate(Candidate.Revision, Entry, Allowed, static_cast<bool>(Candidate.DeriveRegistryInput),
		Imported, bDispatchMissing, ImportDiagnostic);
	if (!bImported)
	{
		PyGILState_Release(Gil);
		if (!ImportDiagnostic.Code.IsEmpty()) { Prepared.Failure.Diagnostics.Add(ImportDiagnostic); }
		else { Prepared.Failure = RejectRevision(SelectImportFailureCode(bDispatchMissing), TEXT("Candidate import or dispatch validation failed.")); }
		return Prepared;
	}
	// Owns the imported objects from here; a rejected candidate releases them under the GIL.
	TSharedRef<FAssetDocumentPythonActiveRevision> Replacement = MakeShared<FAssetDocumentPythonActiveRevision>();
	Replacement->RegistrationProjection = MoveTemp(Imported.RegistrationProjection);
	Replacement->Revision = Candidate.Revision; Replacement->Module = Imported.Module; Imported.Module = nullptr;
	Replacement->Dispatch = Imported.Dispatch; Imported.Dispatch = nullptr; Replacement->AllowedPaths = Allowed;
	PyGILState_Release(Gil);
	FAssetDocumentPrimitiveRegistryBuildInput RegistryInput = Candidate.RegistryInput;
	if (Candidate.DeriveRegistryInput && !Candidate.DeriveRegistryInput(
		Imported.DeclaredRequirements, Replacement->RegistrationProjection, RegistryInput, Prepared.Failure.Diagnostics))
	{
		if (Prepared.Failure.Diagnostics.IsEmpty())
		{
			Prepared.Failure.Diagnostics.Add(MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REVISION_REGISTRY_DERIVATION_FAILED"), TEXT("Registry input could not be derived from the revision declarations.")));
		}
		return Prepared;
	}
	const FAssetDocumentPrimitiveRegistryBuildResult RegistryResult = FAssetDocumentPrimitiveRegistryRuntime::CreateFrozen(RegistryInput);
	if (!RegistryResult.Registry.IsValid()) { Prepared.Failure.Diagnostics = RegistryResult.Diagnostics; return Prepared; }
	Replacement->Registry = RegistryResult.Registry; Prepared.Replacement = Replacement; return Prepared;
}

FAssetDocumentPythonRevisionRuntimeActivationResult InstallPreparedRevision(
	const TSharedRef<FAssetDocumentPythonRevisionState>& State,
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& Handles,
	const TSharedRef<FAssetDocumentPythonActiveRevision>& Replacement)
{
	TSharedPtr<FAssetDocumentPythonActiveRevision> Previous;
	{
		FScopeLock Lock(&State->Mutex);
		if (State->bShutdown) { return RejectRevision(TEXT("AD.RUNTIME.REVISION_INVALID"), TEXT("Revision runtime is shut down.")); }
		if (State->ActiveRequests > 0) { return RejectRevision(TEXT("AD.RUNTIME.REVISION_ACTIVE_REQUEST"), TEXT("An active request pins the current revision.")); }
		Previous = State->Active; State->Active = Replacement;
	}
	if (Previous.IsValid())
	{
		FAssetDocumentHandleInvalidation Event; Event.Reason = EAssetDocumentHandleInvalidationReason::RuntimeRevisionSwap;
		Event.RuntimeRevision = Previous->Revision; Handles->Invalidate(Event);
	}
	FAssetDocumentPythonRevisionRuntimeActivationResult Result; Result.bActivated = true; return Result;
}

FAssetDocumentPythonRevisionPin::FAssetDocumentPythonRevisionPin(
	const TSharedRef<FAssetDocumentPythonRevisionState>& InState,
	const TSharedRef<FAssetDocumentPythonActiveRevision>& InRevision)
	: State(InState), Revision(InRevision) {}

FAssetDocumentPythonRevisionPin::~FAssetDocumentPythonRevisionPin()
{
	FScopeLock Lock(&State->Mutex); check(State->ActiveRequests > 0); --State->ActiveRequests;
}

uint64 FAssetDocumentPythonRevisionPin::GetRevision() const { return Revision->Revision; }
PyObject* FAssetDocumentPythonRevisionPin::GetDispatchCallable() const { return Revision->Dispatch; }
TSharedRef<FAssetDocumentPrimitiveRegistryRuntime> FAssetDocumentPythonRevisionPin::GetRegistry() const { return Revision->Registry.ToSharedRef(); }

PyObject* FAssetDocumentPythonRevisionPin::CallDispatch(PyObject* Request, PyObject* Context,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic) const
{
	FRevisionImportScope Scope(Revision->AllowedPaths);
	if (!Scope.IsValid()) { OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REVISION_IMPORT_FAILED"), TEXT("Restricted import scope could not be entered.")); return nullptr; }
	PyObject* Result = PyObject_CallFunctionObjArgs(Revision->Dispatch, Request, Context, nullptr);
	if (!Scope.AreLoadedOriginsAllowed()) { Py_CLEAR(Result); OutDiagnostic = MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REVISION_IMPORT_FAILED"), TEXT("Dispatch loaded a module outside the allowed roots.")); }
	return Result;
}

FAssetDocumentPythonRevisionRuntime::FAssetDocumentPythonRevisionRuntime(
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& InHandles)
	: Handles(InHandles), State(MakeShared<FAssetDocumentPythonRevisionState>()) {}

FAssetDocumentPythonRevisionRuntime::~FAssetDocumentPythonRevisionRuntime() { Shutdown(); }

FAssetDocumentPythonRevisionRuntimeActivationResult FAssetDocumentPythonRevisionRuntime::TryActivate(
	const FAssetDocumentPythonRevisionRuntimeCandidate& Candidate)
{
	const FAssetDocumentPythonRevisionRuntimeActivationResult Allowed = CheckActivationAllowed(State);
	if (!Allowed.bActivated) { return Allowed; }
	const FRevisionPreparation Prepared = PrepareRevision(Candidate);
	if (!Prepared.Replacement.IsValid()) { return Prepared.Failure; }
	return InstallPreparedRevision(State, Handles, Prepared.Replacement.ToSharedRef());
}

TUniquePtr<FAssetDocumentPythonRevisionPin> FAssetDocumentPythonRevisionRuntime::PinActiveRequest(
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FScopeLock Lock(&State->Mutex);
	if (!State->Active.IsValid() || State->bShutdown) { OutDiagnostics.Add(MakeRevisionDiagnostic(TEXT("AD.RUNTIME.REVISION_INVALID"), TEXT("No active revision is available."))); return nullptr; }
	++State->ActiveRequests;
	return TUniquePtr<FAssetDocumentPythonRevisionPin>(new FAssetDocumentPythonRevisionPin(State, State->Active.ToSharedRef()));
}

uint64 FAssetDocumentPythonRevisionRuntime::GetActiveRevision() const
{
	FScopeLock Lock(&State->Mutex); return State->Active.IsValid() ? State->Active->Revision : 0;
}

FAssetDocumentRuntimeValueDto FAssetDocumentPythonRevisionRuntime::GetActiveRuntimeRegistrationProjection() const
{
	FScopeLock Lock(&State->Mutex);
	if (State->Active.IsValid()) { return State->Active->RegistrationProjection; }
	FAssetDocumentRuntimeValueDto Empty;
	Empty.Kind = EAssetDocumentRuntimeValueKind::Array;
	return Empty;
}

bool FAssetDocumentPythonRevisionRuntime::HasActiveRequests() const
{
	FScopeLock Lock(&State->Mutex); return State->ActiveRequests > 0;
}

void FAssetDocumentPythonRevisionRuntime::Shutdown()
{
	TSharedPtr<FAssetDocumentPythonActiveRevision> Previous;
	{ FScopeLock Lock(&State->Mutex); if (State->bShutdown) { return; } State->bShutdown = true; Previous = State->Active; State->Active.Reset(); }
	if (Previous.IsValid()) { FAssetDocumentHandleInvalidation Event; Event.Reason = EAssetDocumentHandleInvalidationReason::ProviderUnload; Event.RuntimeRevision = Previous->Revision; Handles->Invalidate(Event); }
}
