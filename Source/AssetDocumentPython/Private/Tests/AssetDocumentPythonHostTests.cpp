// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentPythonRuntime.h"

#include "AssetDocumentPythonHostTestDoubles.h"
#include "IPythonScriptPlugin.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include "Python.h"
#include "AssetDocumentPluginPaths.h"
THIRD_PARTY_INCLUDES_END

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
class FRecordingHostProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	FRecordingHostProvider()
		: Descriptor(MakeAssetDocumentHostTestDescriptor())
	{
	}

	FString GetProviderId() const override { return TEXT("editor"); }
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override
	{
		return MakeArrayView(&Descriptor, 1);
	}
	FAssetDocumentProviderAvailability InspectAvailability() const override
	{
		FAssetDocumentProviderAvailability Result;
		Result.ProviderId = TEXT("editor");
		Result.State = EAssetDocumentProviderAvailabilityState::Available;
		return Result;
	}
	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		(void)Handles;
		++CallCount;
		LastInvocation = Invocation;
		LastTransaction = ActiveTransaction;
		bSawGameThread = IsInGameThread();
		bSawGil = PyGILState_Check() != 0;
		FAssetDocumentPrimitiveOutcome Outcome;
		Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
		Outcome.Value = Invocation.ValueArguments;
		if (bRaisePythonError)
		{
			// Native primitive invocation runs with the GIL released, so the fixture must
			// take it before touching interpreter state. The pending error surfaces in the
			// Python dispatch once the native module restores the caller's thread state.
			const PyGILState_STATE Gil = PyGILState_Ensure();
			PyErr_SetString(PyExc_RuntimeError, "fixture provider exception marker");
			PyGILState_Release(Gil);
		}
		return Outcome;
	}

	FAssetDocumentPrimitiveDescriptor Descriptor;
	FAssetDocumentPrimitiveInvocation LastInvocation;
	IAssetDocumentAtomicTransaction* LastTransaction = nullptr;
	int32 CallCount = 0;
	bool bSawGameThread = false;
	bool bSawGil = false;
	bool bRaisePythonError = false;
};

FAssetDocumentPythonRuntimeConfiguration MakeRepositoryConfiguration()
{
	FAssetDocumentPythonRuntimeConfiguration Configuration;
	Configuration.RepositoryRoot =
		GetAssetDocumentPluginBaseDir();
	Configuration.PackageRootRelativePath =
		TEXT("Source/AssetDocumentPython/Private/Tests/Fixtures");
	return Configuration;
}

FAssetDocumentPythonRevisionCandidate MakeCandidate(
	uint64 Revision,
	const TSharedRef<FRecordingHostProvider>& Provider)
{
	FAssetDocumentPythonRevisionCandidate Candidate;
	Candidate.RuntimeRevision = Revision;
	Candidate.EntryModuleRelativePath = TEXT("revision_one.py");
	Candidate.RequiredPrimitives.Add(
		MakeAssetDocumentHostTestRequirement(Provider->Descriptor));
	Candidate.Providers.Add(Provider);
	return Candidate;
}

FAssetDocumentRuntimeValueDto MakeStringValue(const TCHAR* Text)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = Text;
	return Value;
}

FAssetDocumentRuntimeRequestDto MakeRequest(uint64 ExpectedRevision)
{
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.Operation = EAssetDocumentRuntimeOperation::Inspect;
	Request.TargetAssetPath = TEXT("/Game/FHostAbi/FHostAbi.FHostAbi");
	Request.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object;
	Request.CanonicalDocument.ObjectKeys.Add(TEXT("payload"));
	Request.CanonicalDocument.Elements.Add(
		MakeStringValue(TEXT("typed-dispatch-value")));
	Request.ExpectedRuntimeRevision = ExpectedRevision;
	return Request;
}

FAssetDocumentRequestContextMetadata MakeMetadata(
	const FAssetDocumentRuntimeRequestDto& Request,
	uint64 PinnedRevision)
{
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = Request.RequestId;
	Metadata.PinnedRuntimeRevision = PinnedRevision;
	Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	return Metadata;
}

bool HasOnlyDiagnostic(
	const FAssetDocumentRuntimeResultDto& Result,
	FStringView Code)
{
	return Result.Diagnostics.Num() == 1
		&& Result.Diagnostics[0].Code == Code;
}

TArray<uint8> MakeRevisionOneTransportMarker()
{
	const ANSICHAR* Marker =
		"fixture-only-transport-marker:revision-one";
	TArray<uint8> Bytes;
	Bytes.Append(
		reinterpret_cast<const uint8*>(Marker),
		FCStringAnsi::Strlen(Marker));
	return Bytes;
}

void TestProviderProjection(
	FAutomationTestBase& Test,
	const TSharedRef<FRecordingHostProvider>& Provider,
	const FAssetDocumentRuntimeRequestDto& Request,
	const FGuid& TransactionId)
{
	Test.TestTrue(TEXT("dispatch stays on the game thread"), Provider->bSawGameThread);
	// The native module releases the GIL across primitive invocation (reference-scanner
	// workers may need it while the provider runs).
	Test.TestFalse(TEXT("provider runs with the GIL released"), Provider->bSawGil);
	Test.TestEqual(TEXT("provider is called once"), Provider->CallCount, 1);
	Test.TestEqual(TEXT("request id is pinned"), Provider->LastInvocation.RequestId, Request.RequestId);
	Test.TestEqual(TEXT("transaction id is pinned"), Provider->LastInvocation.TransactionId, TransactionId);
	Test.TestEqual(TEXT("runtime revision is pinned"), Provider->LastInvocation.RuntimeRevision, uint64(1));
	Test.TestEqual(TEXT("provider id is exact"), Provider->Descriptor.ProviderId, FString(TEXT("editor")));
	Test.TestEqual(TEXT("phase is pinned"), Provider->LastInvocation.Phase, EAssetDocumentRuntimeRequestPhase::Validate);
	Test.TestEqual(TEXT("request object key count reaches provider"),
		Provider->LastInvocation.ValueArguments.ObjectKeys.Num(), 1);
	Test.TestEqual(TEXT("request value count reaches provider"),
		Provider->LastInvocation.ValueArguments.Elements.Num(), 1);
	if (Provider->LastInvocation.ValueArguments.ObjectKeys.Num() == 1
		&& Provider->LastInvocation.ValueArguments.Elements.Num() == 1)
	{
		Test.TestEqual(TEXT("request object key reaches provider"),
			Provider->LastInvocation.ValueArguments.ObjectKeys[0], FString(TEXT("payload")));
		Test.TestEqual(TEXT("request value reaches provider"),
			Provider->LastInvocation.ValueArguments.Elements[0].StringValue,
			FString(TEXT("typed-dispatch-value")));
	}
}

void TestResultProjection(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Result)
{
	Test.TestEqual(TEXT("result ABI version survives"),
		Result.AbiVersion, uint32{1});
	Test.TestEqual(TEXT("result request id survives"), Result.RequestId, Request.RequestId);
	Test.TestEqual(TEXT("result terminal status survives"),
		Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
	Test.TestEqual(TEXT("result final phase survives"),
		Result.FinalPhase, EAssetDocumentRuntimeRequestPhase::Complete);
	Test.TestEqual(TEXT("result revision survives"), Result.RuntimeRevision, uint64(1));
	Test.TestTrue(TEXT("result value is present"), Result.CanonicalValue.IsSet());
	if (Result.CanonicalValue.IsSet())
	{
		Test.TestEqual(TEXT("result object key count survives"),
			Result.CanonicalValue->ObjectKeys.Num(), 1);
		Test.TestEqual(TEXT("result value count survives"),
			Result.CanonicalValue->Elements.Num(), 1);
		if (Result.CanonicalValue->ObjectKeys.Num() == 1
			&& Result.CanonicalValue->Elements.Num() == 1)
		{
			Test.TestEqual(TEXT("result object key survives"),
				Result.CanonicalValue->ObjectKeys[0], FString(TEXT("payload")));
			Test.TestEqual(TEXT("result value survives"),
				Result.CanonicalValue->Elements[0].StringValue,
				FString(TEXT("typed-dispatch-value")));
		}
	}
	Test.TestEqual(TEXT("successful dispatch has no public diagnostic code"),
		Result.Diagnostics.IsEmpty() ? FString() : Result.Diagnostics[0].Code, FString());
	// The bytes below are fixture-only transport markers; this test asserts field fidelity
	// and G1 tuple shape, not production encoding, hashing, or DocumentCodec semantics.
	Test.TestTrue(TEXT("fixture-only marker bytes survive independently"),
		Result.CanonicalBytes == MakeRevisionOneTransportMarker());
	Test.TestEqual(TEXT("semantic marker survives"), Result.SemanticHash, FString(TEXT("revision-one")));
	Test.TestEqual(TEXT("codec version survives independently"),
		Result.CodecVersion, uint32{1});
	Test.TestEqual(TEXT("changed set survives independently"),
		Result.ChangedSet.Num(), 0);
	Test.TestTrue(TEXT("decoded result passes G1"),
		ValidateAssetDocumentRuntimeResult(Request, Result).bValid);
}

void TestSuccessfulDispatch(
	FAutomationTestBase& Test,
	IAssetDocumentPythonRuntime& Runtime,
	const TSharedRef<FRecordingHostProvider>& Provider)
{
	FAssetDocumentRuntimeRequestDto Request = MakeRequest(1);
	const FGuid TransactionId = FGuid::NewGuid();
	FAssetDocumentPythonRequestContextCreateResult Context =
		Runtime.CreateRequestContext(MakeMetadata(Request, 1), TransactionId, nullptr);
	Test.TestTrue(TEXT("matching context is created"), Context.Context.IsValid());
	if (!Context.Context.IsValid()) { return; }
	const FAssetDocumentRuntimeResultDto Result =
		Runtime.GetHost().Dispatch(Request, *Context.Context);
	TestProviderProjection(Test, Provider, Request, TransactionId);
	TestResultProjection(Test, Request, Result);
}

void TestFailureClosedDispatch(
	FAutomationTestBase& Test,
	IAssetDocumentPythonRuntime& Runtime,
	const TSharedRef<FRecordingHostProvider>& Provider)
{
	FAssetDocumentRuntimeRequestDto WrongRevision = MakeRequest(2);
	FAssetDocumentPythonRequestContextCreateResult WrongContext =
		Runtime.CreateRequestContext(MakeMetadata(WrongRevision, 1), FGuid::NewGuid(), nullptr);
	const int32 CallsBeforeWrongRevision = Provider->CallCount;
	const FAssetDocumentRuntimeResultDto WrongResult =
		Runtime.GetHost().Dispatch(WrongRevision, *WrongContext.Context);
	Test.TestEqual(TEXT("wrong revision is not dispatched"), Provider->CallCount, CallsBeforeWrongRevision);
	Test.TestEqual(TEXT("wrong revision fails"), WrongResult.FinalPhase,
		EAssetDocumentRuntimeRequestPhase::Failed);

	FAssetDocumentRuntimeRequestDto Malformed = MakeRequest(0);
	FAssetDocumentPythonRequestContextCreateResult MalformedContext =
		Runtime.CreateRequestContext(MakeMetadata(Malformed, 1), FGuid::NewGuid(), nullptr);
	const FAssetDocumentRuntimeResultDto MalformedResult =
		Runtime.GetHost().Dispatch(Malformed, *MalformedContext.Context);
	Test.TestEqual(TEXT("malformed Python result fails closed"), MalformedResult.FinalPhase,
		EAssetDocumentRuntimeRequestPhase::Failed);

	FAssetDocumentRuntimeRequestDto ExceptionRequest = MakeRequest(1);
	FAssetDocumentPythonRequestContextCreateResult ExceptionContext =
		Runtime.CreateRequestContext(MakeMetadata(ExceptionRequest, 1), FGuid::NewGuid(), nullptr);
	Provider->bRaisePythonError = true;
	const int32 CallsBeforeException = Provider->CallCount;
	const FAssetDocumentRuntimeResultDto ExceptionResult =
		Runtime.GetHost().Dispatch(ExceptionRequest, *ExceptionContext.Context);
	Provider->bRaisePythonError = false;
	Test.TestEqual(TEXT("Python exception is not retried"), Provider->CallCount, CallsBeforeException + 1);
	Test.TestEqual(TEXT("Python exception returns Failed"), ExceptionResult.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("Python exception returns Failed phase"), ExceptionResult.FinalPhase,
		EAssetDocumentRuntimeRequestPhase::Failed);
	Test.TestTrue(TEXT("Python exception has one stable public diagnostic"),
		HasOnlyDiagnostic(ExceptionResult, TEXT("AD.RUNTIME.PYTHON_EXCEPTION")));
	if (!ExceptionResult.Diagnostics.IsEmpty())
	{
		Test.TestFalse(TEXT("public DTO contains no traceback"),
			ExceptionResult.Diagnostics[0].Message.Contains(TEXT("Traceback")));
	}
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPythonTypedDispatchTest,
	"UEAssetDoc.PythonHost.TypedDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPythonTypedDispatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("test enters on the game thread"), IsInGameThread());
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	TestTrue(TEXT("Engine Python is initialized"),
		Python != nullptr && Python->IsPythonInitialized());
	if (Python == nullptr || !Python->IsPythonInitialized()) { return false; }
	TUniquePtr<IAssetDocumentPythonRuntime> Runtime =
		CreateAssetDocumentPythonRuntime(MakeRepositoryConfiguration());
	TestTrue(TEXT("native binding initializes"), Runtime->InitializePythonBinding().bValid);
	TSharedRef<FRecordingHostProvider> Provider = MakeShared<FRecordingHostProvider>();
	const FAssetDocumentPythonRevisionActivationResult Activation =
		Runtime->TryActivateRevision(MakeCandidate(1, Provider));
	TestTrue(TEXT("revision one activates"), Activation.bActivated);
	TestEqual(TEXT("public active revision is one"), Activation.ActiveRuntimeRevision, uint64(1));
	TestSuccessfulDispatch(*this, *Runtime, Provider);
	TestFailureClosedDispatch(*this, *Runtime, Provider);
	Runtime->ShutdownPythonBinding();
	return true;
}

// Several Python runtimes in one editor process share the typed native module: a runtime that
// shuts its binding down (a test runtime) leaves it to the others (the production bootstrap).
// Before the install was counted, the first shutdown removed the module and every later request
// of the process failed with "RequestContext type is unavailable" (GraphQuery.ResolverRuntime).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPythonSharedNativeModuleTest,
	"UEAssetDoc.PythonHost.SharedNativeModule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPythonSharedNativeModuleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!TestTrue(TEXT("Engine Python is initialized"), Python != nullptr && Python->IsPythonInitialized()))
	{
		return false;
	}
	auto ModuleHasTypes = []()
	{
		const PyGILState_STATE Gil = PyGILState_Ensure();
		PyObject* Module = PyImport_ImportModule("_assetdocument_native");
		const bool bTypes = Module != nullptr && PyObject_HasAttrString(Module, "RequestContext")
			&& PyObject_HasAttrString(Module, "OpaqueHandle");
		Py_XDECREF(Module);
		PyErr_Clear();
		PyGILState_Release(Gil);
		return bTypes;
	};
	TUniquePtr<IAssetDocumentPythonRuntime> Kept = CreateAssetDocumentPythonRuntime(MakeRepositoryConfiguration());
	TestTrue(TEXT("the kept runtime's binding initializes"), Kept->InitializePythonBinding().bValid);
	TSharedRef<FRecordingHostProvider> Provider = MakeShared<FRecordingHostProvider>();
	TestTrue(TEXT("the kept runtime activates revision one"), Kept->TryActivateRevision(MakeCandidate(1, Provider)).bActivated);
	for (int32 Round = 0; Round < 2; ++Round)
	{
		TUniquePtr<IAssetDocumentPythonRuntime> Other = CreateAssetDocumentPythonRuntime(MakeRepositoryConfiguration());
		TestTrue(TEXT("another runtime's binding initializes"), Other->InitializePythonBinding().bValid);
		Other->ShutdownPythonBinding();
		Other.Reset();
		TestTrue(TEXT("the native module keeps its types after another runtime shut down"), ModuleHasTypes());
		Provider->CallCount = 0;
		TestSuccessfulDispatch(*this, *Kept, Provider);
	}
	Kept->ShutdownPythonBinding();
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
