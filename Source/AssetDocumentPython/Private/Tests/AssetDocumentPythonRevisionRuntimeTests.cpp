// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPythonRevisionRuntime.h"

#include "AssetDocumentPythonHost.h"
#include "AssetDocumentPythonHostTestDoubles.h"
#include "AssetDocumentPythonNativeModule.h"
#include "AssetDocumentRequestContextRuntime.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "IPythonScriptPlugin.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include "Python.h"
#include "AssetDocumentPluginPaths.h"
THIRD_PARTY_INCLUDES_END

namespace
{
class FRevisionTestPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	FName GetPayloadTypeId() const override { return TEXT("host.revision.test"); }
	bool IsAlive() const override { return true; }
};

FAssetDocumentPythonRevisionRuntimeCandidate MakeRevisionCandidate(
	uint64 Revision, const FString& Entry,
	const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider)
{
	FAssetDocumentPythonRevisionRuntimeCandidate Candidate;
	Candidate.Revision = Revision;
	Candidate.RepositoryRoot = GetAssetDocumentPluginBaseDir();
	Candidate.PackageRootRelativePath = TEXT("Source/AssetDocumentPython/Private/Tests/Fixtures");
	Candidate.EntryModuleRelativePath = Entry;
	Candidate.RegistryInput.RuntimeRevision = Revision;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		Candidate.RegistryInput.Required.Add(MakeAssetDocumentHostTestRequirement(Descriptor));
	}
	Candidate.RegistryInput.Providers.Add(Provider);
	return Candidate;
}

bool HasDiagnostic(const FAssetDocumentPythonRevisionRuntimeActivationResult& Result, FStringView Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (Diagnostic.Code == Code) { return true; }
	}
	return false;
}

FString TakePythonErrorText()
{
	PyObject* Type = nullptr; PyObject* Value = nullptr; PyObject* Traceback = nullptr;
	PyErr_Fetch(&Type, &Value, &Traceback);
	PyErr_NormalizeException(&Type, &Value, &Traceback);
	PyObject* Text = Value != nullptr ? PyObject_Str(Value) : nullptr;
	const char* Utf8 = Text != nullptr ? PyUnicode_AsUTF8(Text) : nullptr;
	const FString Result = Utf8 != nullptr ? UTF8_TO_TCHAR(Utf8) : TEXT("");
	Py_XDECREF(Text); Py_XDECREF(Type); Py_XDECREF(Value); Py_XDECREF(Traceback);
	return Result;
}

bool HasExactModuleObjects(PyObject* Expected)
{
	PyObject* Current = PyImport_GetModuleDict();
	if (PyDict_Size(Current) != PyDict_Size(Expected)) { return false; }
	PyObject* Key = nullptr; PyObject* Value = nullptr; Py_ssize_t Position = 0;
	while (PyDict_Next(Expected, &Position, &Key, &Value))
	{
		if (PyDict_GetItem(Current, Key) != Value) { return false; }
	}
	return true;
}

bool IsCandidateModuleKey(PyObject* Key)
{
	if (!PyUnicode_Check(Key)) { return false; }
	const char* Utf8 = PyUnicode_AsUTF8(Key);
	if (Utf8 == nullptr) { return false; }
	const FString Name = UTF8_TO_TCHAR(Utf8);
	return Name.StartsWith(TEXT("_assetdocument_revision_"))
		|| Name == TEXT("revision_two");
}

bool HasNoCandidateModuleKeys()
{
	PyObject* Modules = PyImport_GetModuleDict();
	PyObject* Key = nullptr; PyObject* Value = nullptr; Py_ssize_t Position = 0;
	while (PyDict_Next(Modules, &Position, &Key, &Value))
	{
		(void)Value;
		if (IsCandidateModuleKey(Key)) { return false; }
	}
	return true;
}

struct FPythonStateSnapshot final
{
	FPythonStateSnapshot()
	{
		Sys = PyImport_ImportModule("sys"); Path = PyObject_GetAttrString(Sys, "path");
		PathCopy = PySequence_List(Path); ModulesCopy = PyDict_Copy(PyImport_GetModuleDict());
	}
	~FPythonStateSnapshot() { Release(); }
	void Release() { Py_CLEAR(ModulesCopy); Py_CLEAR(PathCopy); Py_CLEAR(Path); Py_CLEAR(Sys); }
	bool Matches() const
	{
		PyObject* CurrentPath = Sys != nullptr ? PyObject_GetAttrString(Sys, "path") : nullptr;
		const bool bMatches = CurrentPath == Path
			&& PyObject_RichCompareBool(Path, PathCopy, Py_EQ) == 1
			&& HasExactModuleObjects(ModulesCopy);
		Py_XDECREF(CurrentPath);
		return bMatches;
	}
	bool HasNoRevisionModules() const
	{
		return HasNoCandidateModuleKeys();
	}
	PyObject* Sys = nullptr; PyObject* Path = nullptr; PyObject* PathCopy = nullptr; PyObject* ModulesCopy = nullptr;
};

class FAmbientImportFixture final
{
public:
	~FAmbientImportFixture() { Release(); }
	bool Initialize()
	{
		Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/F-HOST-ABI/ImportRoots"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Package = FPaths::Combine(Root, TEXT("Package"));
		UserSite = FPaths::Combine(Root, TEXT("UserSite/site-packages"));
		const bool bDirectories = CreateDirectories();
		const bool bFiles = WriteFiles();
		OriginalPythonPath = FPlatformMisc::GetEnvironmentVariable(TEXT("PYTHONPATH"));
		FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONPATH"), *UserSite);
		bActive = true;
		const bool bPath = AppendUserSitePath();
		return bDirectories && bFiles && bPath;
	}
	bool AppendUserSitePath()
	{
		PyObject* Sys = PyImport_ImportModule("sys");
		if (Sys == nullptr) { return false; }
		SysPath = PyObject_GetAttrString(Sys, "path");
		Py_XDECREF(Sys);
		if (SysPath == nullptr) { return false; }
		OriginalSysPath = PySequence_List(SysPath);
		if (OriginalSysPath == nullptr) { return false; }
		PyObject* Row = PyUnicode_FromString(TCHAR_TO_UTF8(*UserSite));
		if (Row == nullptr) { return false; }
		const int32 Result = PyList_Append(SysPath, Row);
		Py_DECREF(Row);
		return Result == 0;
	}
	void Release()
	{
		if (!bActive) { return; }
		if (SysPath != nullptr && OriginalSysPath != nullptr)
		{
			PyList_SetSlice(SysPath, 0, PyList_Size(SysPath), OriginalSysPath);
		}
		Py_CLEAR(OriginalSysPath); Py_CLEAR(SysPath);
		if (PyDict_DelItemString(PyImport_GetModuleDict(), "ambient_only_helper") != 0)
		{
			PyErr_Clear();
		}
		FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONPATH"), *OriginalPythonPath);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		bActive = false;
	}
	FAssetDocumentPythonRevisionRuntimeCandidate MakeAmbientOnlyCandidate(
		uint64 Revision,
		const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider) const
	{
		FAssetDocumentPythonRevisionRuntimeCandidate Candidate =
			MakeRevisionCandidate(Revision, TEXT("import_ambient.py"), Provider);
		Candidate.RepositoryRoot = Root;
		Candidate.PackageRootRelativePath = TEXT("Package");
		return Candidate;
	}
	FAssetDocumentPythonRevisionRuntimeCandidate MakeExceptionCandidate(
		uint64 Revision,
		const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider) const
	{
		FAssetDocumentPythonRevisionRuntimeCandidate Candidate =
			MakeRevisionCandidate(Revision, TEXT("raise_dispatch.py"), Provider);
		Candidate.RepositoryRoot = Root;
		Candidate.PackageRootRelativePath = TEXT("Package");
		return Candidate;
	}
private:
	bool CreateDirectories() const
	{
		return IFileManager::Get().MakeDirectory(*Package, true)
			&& IFileManager::Get().MakeDirectory(*UserSite, true);
	}
	bool WriteFiles() const
	{
		const bool bAmbient = FFileHelper::SaveStringToFile(
			TEXT("REPOSITORY_DEPENDENCY_MARKER = \"ambient\"\n"),
			*FPaths::Combine(UserSite, TEXT("revision_two.py")));
		const bool bHelper = FFileHelper::SaveStringToFile(
			TEXT("AMBIENT_ONLY = True\n"),
			*FPaths::Combine(UserSite, TEXT("ambient_only_helper.py")));
		const bool bCandidate = FFileHelper::SaveStringToFile(
			TEXT("import ambient_only_helper\n\ndef dispatch(request, context):\n    return {}\n"),
			*FPaths::Combine(Package, TEXT("import_ambient.py")));
		const bool bException = FFileHelper::SaveStringToFile(
			TEXT("def dispatch(request, context):\n"
				"    raise RuntimeError(\"fixture dispatch exception marker\")\n"),
			*FPaths::Combine(Package, TEXT("raise_dispatch.py")));
		return bAmbient && bHelper && bCandidate && bException;
	}
	FString Root; FString Package; FString UserSite; FString OriginalPythonPath;
	PyObject* SysPath = nullptr; PyObject* OriginalSysPath = nullptr; bool bActive = false;
};

bool IsEngineStdlibModule(PyObject* Module)
{
	PyObject* File = Module != nullptr ? PyObject_GetAttrString(Module, "__file__") : nullptr;
	const char* Utf8 = File != nullptr && PyUnicode_Check(File) ? PyUnicode_AsUTF8(File) : nullptr;
	FString Origin = Utf8 != nullptr ? UTF8_TO_TCHAR(Utf8) : TEXT("");
	Py_XDECREF(File);
#if PLATFORM_WINDOWS
	FString EngineStdlib = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::EngineDir(), TEXT("Binaries/ThirdParty/Python3/Win64/Lib")));
#else
	FString EngineStdlib = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::EngineDir(), TEXT("Binaries/ThirdParty/Python3/Mac/lib/python3.11")));
#endif
	FPaths::NormalizeFilename(Origin); FPaths::NormalizeFilename(EngineStdlib);
	return Origin.StartsWith(EngineStdlib + TEXT("/"));
}

void TestRestoredPythonState(
	FAutomationTestBase& Test,
	const TCHAR* Label,
	const FPythonStateSnapshot& Snapshot)
{
	Test.TestTrue(Label, Snapshot.Matches());
	Test.TestTrue(TEXT("candidate and repository imports do not leak"),
		Snapshot.HasNoRevisionModules());
}

TOptional<FAssetDocumentOpaqueHandle> IssueRevisionHandle(
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& Handles,
	uint64 Revision)
{
	FAssetDocumentHandleResolverBinding Binding;
	Binding.RequestId = FGuid::NewGuid(); Binding.TransactionId = FGuid::NewGuid();
	Binding.RuntimeRevision = Revision; Binding.ProviderId = TEXT("editor");
	FAssetDocumentPrimitiveHandleResultSchema& Schema = Binding.AllowedResults.AddDefaulted_GetRef();
	Schema.Name = TEXT("target"); Schema.Kind = EAssetDocumentHandleKind::Object;
	Schema.GrantedAccess = EAssetDocumentHandleAccess::Mutable;
	Schema.Cardinality = EAssetDocumentHandleResultCardinality::ExactlyOne;
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session =
		Handles->CreateInvocationSession(Binding);
	FAssetDocumentIssuedHandleResult Issued = Session->Issue(
		EAssetDocumentHandleKind::Object, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FRevisionTestPayload>());
	if (!Issued.Handle.IsSet()) { return {}; }
	FAssetDocumentPrimitiveHandleResult Result; Result.Name = TEXT("target");
	Result.Handles.Add(Issued.Handle.GetValue());
	if (!Session->MarkValidatedReturnedHandles(MakeArrayView(&Result, 1)).bValid) { return {}; }
	Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Succeeded);
	return Issued.Handle;
}

PyObject* MakeRevisionDispatchRequest(const FGuid& RequestId, uint64 Revision)
{
	const FString RequestIdText = RequestId.ToString(EGuidFormats::DigitsWithHyphensLower);
	PyObject* Request = PyDict_New();
	PyObject* CanonicalDocument = PyDict_New();
	PyDict_SetItemString(CanonicalDocument, "payload", PyUnicode_FromString("revision-dispatch"));
	PyDict_SetItemString(Request, "request_id", PyUnicode_FromString(TCHAR_TO_UTF8(*RequestIdText)));
	PyDict_SetItemString(Request, "expected_runtime_revision", PyLong_FromUnsignedLongLong(Revision));
	PyDict_SetItemString(Request, "canonical_document", CanonicalDocument);
	Py_DECREF(CanonicalDocument);
	return Request;
}

FAssetDocumentRuntimeRequestDto MakeExceptionDispatchRequest(uint64 Revision)
{
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.Operation = EAssetDocumentRuntimeOperation::Inspect;
	Request.TargetAssetPath = TEXT("/Game/FHostAbi/FHostAbi.FHostAbi");
	Request.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object;
	Request.CanonicalDocument.ObjectKeys.Add(TEXT("payload"));
	FAssetDocumentRuntimeValueDto& Value =
		Request.CanonicalDocument.Elements.AddDefaulted_GetRef();
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = TEXT("exception-dispatch");
	Request.ExpectedRuntimeRevision = Revision;
	return Request;
}

void TestPinnedRevisionDispatch(
	FAutomationTestBase& Test,
	FAssetDocumentPythonRevisionPin& Pin,
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& Handles)
{
	const FGuid RequestId = FGuid::NewGuid();
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = RequestId; Metadata.PinnedRuntimeRevision = Pin.GetRevision();
	Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	FAssetDocumentNativeRequestContextRuntime Context(
		Metadata, FGuid::NewGuid(), Pin.GetRegistry(), Handles, nullptr);
	TSharedRef<FAssetDocumentPythonDispatchBindingState> Binding =
		AssetDocumentPythonOpenDispatchBinding(Context);
	PyObject* ContextProxy = AssetDocumentPythonCreateRequestContextProxy(Binding);
	PyObject* Request = MakeRevisionDispatchRequest(RequestId, Pin.GetRevision());
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	PyObject* Result = Pin.CallDispatch(Request, ContextProxy, Diagnostic);
	Test.TestNotNull(TEXT("revision one dispatch loads repository dependency and Engine json"), Result);
	PyObject* Hash = Result != nullptr ? PyDict_GetItemString(Result, "semantic_hash") : nullptr;
	const char* Utf8 = Hash != nullptr ? PyUnicode_AsUTF8(Hash) : nullptr;
	Test.TestEqual(TEXT("repository fixture wins ambient module precedence"),
		Utf8 != nullptr ? FString(UTF8_TO_TCHAR(Utf8)) : FString(),
		FString(TEXT("revision-one")));
	AssetDocumentPythonCloseDispatchBinding(Binding);
	Py_XDECREF(Result); Py_DECREF(Request); Py_DECREF(ContextProxy); PyErr_Clear();
}

void TestDispatchExceptionRestoration(
	FAutomationTestBase& Test,
	FAssetDocumentPythonRevisionRuntime& Runtime,
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& Handles,
	const TSharedRef<FAssetDocumentHostTestProvider>& Provider,
	const FAmbientImportFixture& Ambient,
	const FPythonStateSnapshot& Snapshot)
{
	const FAssetDocumentPythonRevisionRuntimeActivationResult Activation =
		Runtime.TryActivate(Ambient.MakeExceptionCandidate(2, Provider));
	Test.TestTrue(TEXT("exception fixture activates as revision two"), Activation.bActivated);
	Test.TestEqual(TEXT("exception fixture preserves active revision two"),
		Runtime.GetActiveRevision(), uint64{2});
	TestRestoredPythonState(Test, TEXT("exception activation restores exact Python state"), Snapshot);
	TArray<FAssetDocumentRuntimeDiagnostic> PinDiagnostics;
	TUniquePtr<FAssetDocumentPythonRevisionPin> Pin =
		Runtime.PinActiveRequest(PinDiagnostics);
	Test.TestTrue(TEXT("exception revision can be pinned"), Pin.IsValid());
	if (!Pin.IsValid()) { return; }
	TSharedRef<FAssetDocumentPrimitiveRegistryRuntime> Registry = Pin->GetRegistry();
	Pin.Reset();
	const FAssetDocumentRuntimeRequestDto Request = MakeExceptionDispatchRequest(2);
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = Request.RequestId;
	Metadata.PinnedRuntimeRevision = 2;
	Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	FAssetDocumentNativeRequestContextRuntime Context(
		Metadata, FGuid::NewGuid(), Registry, Handles, nullptr);
	FAssetDocumentPythonHost Host(Runtime);
	const FAssetDocumentRuntimeResultDto Result = Host.Dispatch(Request, Context);
	Test.TestEqual(TEXT("dispatch exception fails"), Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("dispatch exception has one diagnostic"),
		Result.Diagnostics.Num(), 1);
	if (!Result.Diagnostics.IsEmpty())
	{
		Test.TestEqual(TEXT("dispatch exception preserves frozen diagnostic"),
			Result.Diagnostics[0].Code, FString(TEXT("AD.RUNTIME.PYTHON_EXCEPTION")));
	}
	TestRestoredPythonState(Test, TEXT("dispatch exception restores exact Python state"), Snapshot);
}

void TestProviderAndShutdownInvalidation(
	FAutomationTestBase& Test,
	FAssetDocumentPythonRevisionRuntime& Runtime,
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& Handles)
{
	const TOptional<FAssetDocumentOpaqueHandle> ProviderHandle =
		IssueRevisionHandle(Handles, 2);
	FAssetDocumentHandleInvalidation ProviderUnload;
	ProviderUnload.Reason = EAssetDocumentHandleInvalidationReason::ProviderUnload;
	ProviderUnload.ProviderId = TEXT("editor");
	Handles->Invalidate(ProviderUnload);
	Test.TestTrue(TEXT("provider unload handle was issued"), ProviderHandle.IsSet());
	Test.TestFalse(TEXT("provider unload invalidates matching handle"),
		ProviderHandle.IsSet() && Handles->IsIssuedAndLive(ProviderHandle.GetValue()));
	const TOptional<FAssetDocumentOpaqueHandle> ShutdownHandle =
		IssueRevisionHandle(Handles, 2);
	Runtime.Shutdown();
	Test.TestTrue(TEXT("shutdown handle was issued"), ShutdownHandle.IsSet());
	Test.TestFalse(TEXT("revision shutdown invalidates matching handle"),
		ShutdownHandle.IsSet() && Handles->IsIssuedAndLive(ShutdownHandle.GetValue()));
}

void TestRevisionSwapInvalidation(
	FAutomationTestBase& Test,
	FAssetDocumentPythonRevisionRuntime& Runtime,
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& Handles,
	const TSharedRef<FAssetDocumentHostTestProvider>& Provider,
	const FPythonStateSnapshot& Snapshot)
{
	const TOptional<FAssetDocumentOpaqueHandle> OldHandle =
		IssueRevisionHandle(Handles, 1);
	Test.TestTrue(TEXT("old revision handle was issued"), OldHandle.IsSet());
	Test.TestTrue(TEXT("old revision handle starts live"),
		OldHandle.IsSet() && Handles->IsIssuedAndLive(OldHandle.GetValue()));
	const FAssetDocumentPythonRevisionRuntimeActivationResult Result =
		Runtime.TryActivate(MakeRevisionCandidate(2, TEXT("revision_two.py"), Provider));
	Test.TestTrue(TEXT("revision two activates after pin release"), Result.bActivated);
	Test.TestEqual(TEXT("active revision becomes two"), Runtime.GetActiveRevision(), uint64{2});
	Test.TestFalse(TEXT("revision swap makes old handle stale"),
		OldHandle.IsSet() && Handles->IsIssuedAndLive(OldHandle.GetValue()));
	TestRestoredPythonState(Test, TEXT("revision swap restores exact Python state"), Snapshot);
}

void TestRejectedRevisionCandidates(
	FAutomationTestBase& Test,
	FAssetDocumentPythonRevisionRuntime& Runtime,
	const TSharedRef<FAssetDocumentHostTestProvider>& Provider,
	const FAmbientImportFixture& Ambient,
	const FPythonStateSnapshot& Snapshot)
{
	FAssetDocumentPythonRevisionRuntimeActivationResult Result = Runtime.TryActivate(
		MakeRevisionCandidate(3, TEXT("revision_invalid.py"), Provider));
	Test.TestFalse(TEXT("non-callable dispatch is rejected"), Result.bActivated);
	Test.TestTrue(TEXT("dispatch rejection is stable"),
		HasDiagnostic(Result, TEXT("AD.RUNTIME.REVISION_DISPATCH_MISSING")));
	TestRestoredPythonState(Test, TEXT("dispatch rejection restores exact Python state"), Snapshot);
	Result = Runtime.TryActivate(MakeRevisionCandidate(4, TEXT("missing.py"), Provider));
	Test.TestFalse(TEXT("missing module is rejected"), Result.bActivated);
	Test.TestTrue(TEXT("missing import rejection is stable"),
		HasDiagnostic(Result, TEXT("AD.RUNTIME.REVISION_IMPORT_FAILED")));
	TestRestoredPythonState(Test, TEXT("missing import restores exact Python state"), Snapshot);
	FAssetDocumentPrimitiveDescriptor Incomplete = MakeAssetDocumentHostTestDescriptor();
	Incomplete.Admission.PublicHeader.Reset();
	TSharedRef<FAssetDocumentHostTestProvider> IncompleteProvider =
		MakeShared<FAssetDocumentHostTestProvider>(
			TEXT("editor"), TArray<FAssetDocumentPrimitiveDescriptor>{Incomplete});
	Result = Runtime.TryActivate(
		MakeRevisionCandidate(5, TEXT("revision_two.py"), IncompleteProvider));
	Test.TestFalse(TEXT("incomplete descriptor cannot activate"), Result.bActivated);
	Test.TestTrue(TEXT("incomplete descriptor preserves registry diagnostic"),
		HasDiagnostic(Result, TEXT("PrimitiveDescriptor.InvalidAdmission")));
	Test.TestEqual(TEXT("registry rejection preserves active revision two"),
		Runtime.GetActiveRevision(), uint64{2});
	TestRestoredPythonState(Test, TEXT("registry rejection preserves exact Python state"), Snapshot);
	Result = Runtime.TryActivate(Ambient.MakeAmbientOnlyCandidate(6, Provider));
	Test.TestFalse(TEXT("ambient-only user-site import is rejected"), Result.bActivated);
	Test.TestTrue(TEXT("ambient-only rejection is a stable import failure"),
		HasDiagnostic(Result, TEXT("AD.RUNTIME.REVISION_IMPORT_FAILED")));
	Test.TestEqual(TEXT("candidate failures preserve last known good"),
		Runtime.GetActiveRevision(), uint64{2});
	TestRestoredPythonState(Test, TEXT("ambient-only rejection restores exact Python state"), Snapshot);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPythonRevisionLastKnownGoodTest,
	"UEAssetDoc.PythonHost.RevisionLastKnownGood",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPythonRevisionLastKnownGoodTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	TestTrue(TEXT("Engine Python is initialized"), Python != nullptr && Python->IsPythonInitialized());
	if (Python == nullptr || !Python->IsPythonInitialized()) { return false; }
	PyGILState_STATE Gil = PyGILState_Ensure();
	TestTrue(TEXT("typed native module installs"),
		AssetDocumentPythonInstallNativeModule().bValid);
	FAmbientImportFixture Ambient;
	TestTrue(TEXT("ambient import fixture initializes"), Ambient.Initialize());
	PyObject* AmbientModule = PyImport_ImportModule("ambient_only_helper");
	const FString AmbientImportError =
		AmbientModule == nullptr ? TakePythonErrorText() : FString();
	TestNotNull(*FString::Printf(TEXT("ambient-only module is initially available: %s"),
		*AmbientImportError), AmbientModule);
	PyObject* Json = PyImport_ImportModule("json");
	TestTrue(TEXT("json resolves from Engine stdlib"), IsEngineStdlibModule(Json));
	Py_XDECREF(Json);
	FPythonStateSnapshot PythonState;
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Handles = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	TSharedRef<FAssetDocumentHostTestProvider> Provider = MakeShared<FAssetDocumentHostTestProvider>(
		TEXT("editor"), TArray<FAssetDocumentPrimitiveDescriptor>{MakeAssetDocumentHostTestDescriptor()});
	FAssetDocumentPythonRevisionRuntime Runtime(Handles);
	FAssetDocumentPythonRevisionRuntimeActivationResult Result = Runtime.TryActivate(
		MakeRevisionCandidate(1, TEXT("revision_one.py"), Provider));
	TestTrue(TEXT("revision one activates"), Result.bActivated);
	TestRestoredPythonState(*this, TEXT("successful activation restores exact Python state"), PythonState);
	TestEqual(TEXT("active revision is one"), Runtime.GetActiveRevision(), uint64(1));
	TArray<FAssetDocumentRuntimeDiagnostic> PinDiagnostics;
	TUniquePtr<FAssetDocumentPythonRevisionPin> Pin = Runtime.PinActiveRequest(PinDiagnostics);
	Result = Runtime.TryActivate(MakeRevisionCandidate(2, TEXT("revision_two.py"), Provider));
	TestFalse(TEXT("active request blocks activation"), Result.bActivated);
	TestTrue(TEXT("active request has stable diagnostic"), HasDiagnostic(Result, TEXT("AD.RUNTIME.REVISION_ACTIVE_REQUEST")));
	TestPinnedRevisionDispatch(*this, *Pin, Handles);
	TestRestoredPythonState(*this, TEXT("dispatch restores exact Python state"), PythonState);
	Pin.Reset();
	TestRevisionSwapInvalidation(*this, Runtime, Handles, Provider, PythonState);
	TestRejectedRevisionCandidates(*this, Runtime, Provider, Ambient, PythonState);
	TestDispatchExceptionRestoration(
		*this, Runtime, Handles, Provider, Ambient, PythonState);
	TestProviderAndShutdownInvalidation(*this, Runtime, Handles);
	TestEqual(TEXT("shutdown clears active revision"), Runtime.GetActiveRevision(), uint64(0));
	TestRestoredPythonState(*this, TEXT("shutdown preserves exact Python state"), PythonState);
	PythonState.Release();
	Py_XDECREF(AmbientModule);
	AssetDocumentPythonRemoveNativeModule();
	Ambient.Release();
	PyGILState_Release(Gil);
	return true;
}

namespace
{
class FRegistrationProjectionFixture final
{
public:
	FRegistrationProjectionFixture()
	{
		Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/R1/RegistrationProjection"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Package")), true);
	}
	~FRegistrationProjectionFixture() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
	bool Write(const FString& Name, const FString& Source) const
	{
		return FFileHelper::SaveStringToFile(Source, *FPaths::Combine(Root, TEXT("Package"), Name));
	}
	FAssetDocumentPythonRevisionRuntimeCandidate Candidate(uint64 Revision, const FString& Name,
		const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider) const
	{
		auto Out = MakeRevisionCandidate(Revision, Name, Provider);
		Out.RepositoryRoot = Root; Out.PackageRootRelativePath = TEXT("Package");
		return Out;
	}
private:
	FString Root;
};

const TCHAR* RegistrationProjectionFixtureSource = TEXT(
	"row = {'Class': '/Script/Test.Asset', 'BodySections': ('Settings',),\n"
	"       'RuntimeRegistration': {'LifecycleId': 'test.lifecycle.v1', 'TargetPolicy': 'UpdateOnly',\n"
	"       'RequiredPrimitives': ({'PrimitiveId': 'test.read.v1', 'Version': 1, 'RequiredByRouteId': 'test'},)}}\n"
	"class Dispatch:\n"
	"    calls = 0\n"
	"    def __call__(self, request, context):\n"
	"        return self.calls\n"
	"    def describe_runtime_registrations(self):\n"
	"        self.calls += 1\n"
	"        return (row,)\n"
	"dispatch = Dispatch()\n");

void TestRegistrationProjectionSnapshot(FAutomationTestBase& Test,
	FAssetDocumentPythonRevisionRuntime& Runtime)
{
	FAssetDocumentRuntimeValueDto First = Runtime.GetActiveRuntimeRegistrationProjection();
	Test.TestEqual(TEXT("registration projection is an array"), First.Kind, EAssetDocumentRuntimeValueKind::Array);
	Test.TestEqual(TEXT("one registration is captured"), First.Elements.Num(), 1);
	First.Elements.Reset();
	Test.TestEqual(TEXT("getter returns an independent copy"), Runtime.GetActiveRuntimeRegistrationProjection().Elements.Num(), 1);
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	auto Pin = Runtime.PinActiveRequest(Diagnostics);
	Test.TestTrue(TEXT("captured revision can be pinned"), Pin.IsValid());
	if (!Pin.IsValid()) { return; }
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	PyObject* Calls = Pin->CallDispatch(Py_None, Py_None, Diagnostic);
	Test.TestNotNull(TEXT("callable object dispatch remains callable"), Calls);
	if (Calls != nullptr)
	{
		Test.TestEqual(TEXT("describe executes exactly once despite repeated getter calls"), PyLong_AsLong(Calls), long{1});
	}
	Py_XDECREF(Calls);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPythonRevisionRegistrationProjectionTest,
	"UEAssetDoc.PythonHost.RevisionRegistrationProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPythonRevisionRegistrationProjectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!TestTrue(TEXT("Engine Python is initialized"), Python != nullptr && Python->IsPythonInitialized())) { return false; }
	PyGILState_STATE Gil = PyGILState_Ensure();
	{
		FRegistrationProjectionFixture Fixture;
		FPythonStateSnapshot PythonState;
		auto Handles = MakeShared<FAssetDocumentHandleRegistryRuntime>();
		auto Provider = MakeShared<FAssetDocumentHostTestProvider>(TEXT("editor"),
			TArray<FAssetDocumentPrimitiveDescriptor>{MakeAssetDocumentHostTestDescriptor()});
		FAssetDocumentPythonRevisionRuntime Runtime(Handles);
		TestEqual(TEXT("no active revision has empty-array projection"), Runtime.GetActiveRuntimeRegistrationProjection().Kind,
			EAssetDocumentRuntimeValueKind::Array);
		TestTrue(TEXT("write callable fixture"), Fixture.Write(TEXT("good.py"), RegistrationProjectionFixtureSource));
		TestTrue(TEXT("callable registration activates"), Runtime.TryActivate(Fixture.Candidate(1, TEXT("good.py"), Provider)).bActivated);
		TestRegistrationProjectionSnapshot(*this, Runtime);
		TestTrue(TEXT("capture restores Python import state"), PythonState.Matches());

		struct FRejectedProjection { const TCHAR* Name; const TCHAR* Code; const TCHAR* Diagnostic; };
		const FRejectedProjection Cases[] = {
			{TEXT("noncallable"), TEXT("dispatch.describe_runtime_registrations = 17\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("exception"), TEXT("def fail():\n    raise RuntimeError('projection failed')\ndispatch.describe_runtime_registrations = fail\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_CALL_FAILED")},
			{TEXT("lookup_exception"), TEXT("def fail_lookup(self):\n    raise AttributeError('declared descriptor failed')\nDispatch.describe_runtime_registrations = property(fail_lookup)\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_CALL_FAILED")},
			{TEXT("arity"), TEXT("dispatch.describe_runtime_registrations = lambda required: ()\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_CALL_FAILED")},
			{TEXT("nonarray"), TEXT("dispatch.describe_runtime_registrations = lambda: row\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("malformed"), TEXT("dispatch.describe_runtime_registrations = lambda: ({'Class': '/Script/Test.Asset'},)\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("extra_row"), TEXT("row['Extra'] = None\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("duplicate"), TEXT("dispatch.describe_runtime_registrations = lambda: (row, row)\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("existing_create"), TEXT("del row['RuntimeRegistration']['LifecycleId']\nrow['RuntimeRegistration']['TargetPolicy'] = 'CreateOrUpdate'\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("existing_extra"), TEXT("del row['RuntimeRegistration']['LifecycleId']\nrow['RuntimeRegistration']['Unknown'] = 'x'\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("existing_missing_requirements"), TEXT("del row['RuntimeRegistration']['LifecycleId']\ndel row['RuntimeRegistration']['RequiredPrimitives']\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("policy"), TEXT("row['RuntimeRegistration']['TargetPolicy'] = 'unknown'\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("requirement"), TEXT("row['RuntimeRegistration']['RequiredPrimitives'][0]['Version'] = True\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("duplicate_requirement"), TEXT("row['RuntimeRegistration']['RequiredPrimitives'] *= 2\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("body"), TEXT("row['BodySections'] = ('Settings', 'Settings')\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("host_shape"), TEXT("row['BodySections'] = ['Settings']\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
			{TEXT("host_depth"), TEXT("nested = ()\nfor _ in range(100):\n    nested = (nested,)\nrow['Extra'] = nested\n"), TEXT("AD.RUNTIME.REGISTRATION_PROJECTION_INVALID")},
		};
		uint64 Revision = 2;
		for (const FRejectedProjection& Case : Cases)
		{
			const FString Name = FString(Case.Name) + TEXT(".py");
			TestTrue(*Name, Fixture.Write(Name, FString(RegistrationProjectionFixtureSource) + Case.Code));
			const auto Result = Runtime.TryActivate(Fixture.Candidate(Revision++, Name, Provider));
			TestFalse(*FString::Printf(TEXT("%s candidate is rejected"), Case.Name), Result.bActivated);
			TestTrue(*FString::Printf(TEXT("%s preserves exact diagnostic"), Case.Name), HasDiagnostic(Result, Case.Diagnostic));
			TestEqual(TEXT("rejection preserves previous revision"), Runtime.GetActiveRevision(), uint64{1});
			TestRegistrationProjectionSnapshot(*this, Runtime);
			TestTrue(TEXT("rejection restores Python import state"), PythonState.Matches());
			TestFalse(TEXT("rejection clears Python exception"), PyErr_Occurred() != nullptr);
		}
		TestTrue(TEXT("write exact existing-only callable"), Fixture.Write(TEXT("existing.py"), FString(RegistrationProjectionFixtureSource)
			+ TEXT("del row['RuntimeRegistration']['LifecycleId']\n")));
		TestTrue(TEXT("two-field UpdateOnly registration activates"), Runtime.TryActivate(Fixture.Candidate(Revision++, TEXT("existing.py"), Provider)).bActivated);
		TestRegistrationProjectionSnapshot(*this, Runtime);
		TestTrue(TEXT("write plain callable fixture"), Fixture.Write(TEXT("plain.py"), TEXT("def dispatch(request, context):\n    return {}\n")));
		TestTrue(TEXT("plain callable activates"), Runtime.TryActivate(Fixture.Candidate(Revision, TEXT("plain.py"), Provider)).bActivated);
		const auto Empty = Runtime.GetActiveRuntimeRegistrationProjection();
		TestEqual(TEXT("plain callable captures Array"), Empty.Kind, EAssetDocumentRuntimeValueKind::Array);
		TestTrue(TEXT("plain callable clears previous projection"), Empty.Elements.IsEmpty());
		Runtime.Shutdown();
		TestTrue(TEXT("shutdown has empty projection"), Runtime.GetActiveRuntimeRegistrationProjection().Elements.IsEmpty());
		PythonState.Release();
	}
	PyGILState_Release(Gil);
	return true;
}

namespace
{
const TCHAR* DeclaredRequirementFixtureSource = TEXT(
	"def dispatch(request, context):\n"
	"    return {}\n"
	"rows = ({'PrimitiveId': 'editor.host_test.echo', 'Version': 1, 'RequiredByRouteId': 'host.test.route'},)\n"
	"dispatch.describe_required_primitives = lambda: rows\n");

struct FDerivationProbe
{
	int32 Calls = 0;
	TArray<FAssetDocumentPrimitiveRequirement> Declared;
	bool bReject = false;
};

FAssetDocumentPythonRevisionRuntimeCandidate MakeDerivedCandidate(
	FAssetDocumentPythonRevisionRuntimeCandidate Candidate,
	const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider, FDerivationProbe& Probe)
{
	// Static input is deliberately empty: only the derived input may freeze.
	Candidate.RegistryInput.Required.Reset();
	Candidate.RegistryInput.Providers.Reset();
	Candidate.DeriveRegistryInput = [&Probe, Provider](TConstArrayView<FAssetDocumentPrimitiveRequirement> Declared,
		const FAssetDocumentRuntimeValueDto& Registrations, FAssetDocumentPrimitiveRegistryBuildInput& InOutInput,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
	{
		(void)Registrations;
		++Probe.Calls;
		Probe.Declared = TArray<FAssetDocumentPrimitiveRequirement>(Declared);
		if (Probe.bReject)
		{
			FAssetDocumentRuntimeDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
			Diagnostic.Code = TEXT("PrimitiveRegistry.MissingRequiredPrimitive");
			Diagnostic.Path = TEXT("/PrimitiveRegistry/RequiredPrimitives/0");
			return false;
		}
		InOutInput.Required = Probe.Declared;
		InOutInput.Providers = {Provider};
		return true;
	};
	return Candidate;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPythonRevisionDerivedRegistryInputTest,
	"UEAssetDoc.PythonHost.RevisionDerivedRegistryInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPythonRevisionDerivedRegistryInputTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!TestTrue(TEXT("Engine Python is initialized"), Python != nullptr && Python->IsPythonInitialized())) { return false; }
	PyGILState_STATE Gil = PyGILState_Ensure();
	{
		FRegistrationProjectionFixture Fixture;
		FPythonStateSnapshot PythonState;
		auto Handles = MakeShared<FAssetDocumentHandleRegistryRuntime>();
		auto Provider = MakeShared<FAssetDocumentHostTestProvider>(TEXT("editor"),
			TArray<FAssetDocumentPrimitiveDescriptor>{MakeAssetDocumentHostTestDescriptor()});
		FAssetDocumentPythonRevisionRuntime Runtime(Handles);
		TestTrue(TEXT("write declaring fixture"), Fixture.Write(TEXT("declared.py"), DeclaredRequirementFixtureSource));
		FDerivationProbe Probe;
		auto Result = Runtime.TryActivate(MakeDerivedCandidate(Fixture.Candidate(1, TEXT("declared.py"), Provider), Provider, Probe));
		for (const auto& D : Result.Diagnostics) { AddError(D.Code + TEXT(": ") + D.Message); }
		TestTrue(TEXT("derived registry input activates"), Result.bActivated);
		TestEqual(TEXT("derivation runs once"), Probe.Calls, 1);
		if (TestEqual(TEXT("declared rows reach the callback"), Probe.Declared.Num(), 1))
		{
			TestEqual(TEXT("declared id"), Probe.Declared[0].PrimitiveId, FString(TEXT("editor.host_test.echo")));
			TestTrue(TEXT("declared version"), Probe.Declared[0].Version == 1);
			TestEqual(TEXT("declared route"), Probe.Declared[0].RequiredByRouteId, FString(TEXT("host.test.route")));
		}
		{
			TArray<FAssetDocumentRuntimeDiagnostic> PinDiagnostics;
			auto Pin = Runtime.PinActiveRequest(PinDiagnostics);
			if (TestTrue(TEXT("derived revision pins"), Pin.IsValid()))
			{
				TestTrue(TEXT("frozen registry resolves the declared row"),
					Pin->GetRegistry()->Resolve(Probe.Declared[0]).bResolved);
			}
		}
		TestTrue(TEXT("derivation restores Python import state"), PythonState.Matches());

		struct FRejected { const TCHAR* Name; const TCHAR* Source; const TCHAR* Diagnostic; bool bCallbackRuns; bool bReject; };
		const FRejected Cases[] = {
			{TEXT("absent"), TEXT("def dispatch(request, context):\n    return {}\n"), TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_INVALID"), false, false},
			{TEXT("raises"), TEXT("def fail():\n    raise RuntimeError('declarations failed')\ndispatch.describe_required_primitives = fail\n"), TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_CALL_FAILED"), false, false},
			{TEXT("list_wire"), TEXT("dispatch.describe_required_primitives = lambda: list(rows)\n"), TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_INVALID"), false, false},
			{TEXT("extra_key"), TEXT("rows[0]['Extra'] = 1\n"), TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_INVALID"), false, false},
			{TEXT("bool_version"), TEXT("rows[0]['Version'] = True\n"), TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_INVALID"), false, false},
			{TEXT("empty_route"), TEXT("rows[0]['RequiredByRouteId'] = ''\n"), TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_INVALID"), false, false},
			{TEXT("duplicate"), TEXT("rows = rows + rows\n"), TEXT("AD.RUNTIME.REQUIRED_PRIMITIVES_INVALID"), false, false},
			{TEXT("callback_rejects"), TEXT(""), TEXT("PrimitiveRegistry.MissingRequiredPrimitive"), true, true},
		};
		uint64 Revision = 2;
		for (const FRejected& Case : Cases)
		{
			const FString Name = FString(Case.Name) + TEXT(".py");
			const FString Source = FString(Case.Name) == TEXT("absent") ? FString(Case.Source)
				: FString(DeclaredRequirementFixtureSource) + Case.Source;
			TestTrue(*Name, Fixture.Write(Name, Source));
			FDerivationProbe CaseProbe; CaseProbe.bReject = Case.bReject;
			const auto Rejected = Runtime.TryActivate(MakeDerivedCandidate(Fixture.Candidate(Revision++, Name, Provider), Provider, CaseProbe));
			TestFalse(*FString::Printf(TEXT("%s is rejected"), Case.Name), Rejected.bActivated);
			TestTrue(*FString::Printf(TEXT("%s has exact diagnostic"), Case.Name), HasDiagnostic(Rejected, Case.Diagnostic));
			TestEqual(*FString::Printf(TEXT("%s callback invocation"), Case.Name), CaseProbe.Calls, Case.bCallbackRuns ? 1 : 0);
			TestEqual(TEXT("rejection keeps last known good"), Runtime.GetActiveRevision(), uint64{1});
			TestTrue(TEXT("rejection restores Python import state"), PythonState.Matches());
			TestFalse(TEXT("rejection clears Python exception"), PyErr_Occurred() != nullptr);
		}
		Runtime.Shutdown();
		PythonState.Release();
	}
	PyGILState_Release(Gil);
	return true;
}
