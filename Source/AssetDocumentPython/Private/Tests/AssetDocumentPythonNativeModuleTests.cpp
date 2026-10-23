// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHandleRegistryRuntime.h"
#include "AssetDocumentPrimitiveRegistryRuntime.h"
#include "AssetDocumentPythonDtoCodec.h"
#include "AssetDocumentPythonHostTestDoubles.h"
#include "AssetDocumentPythonNativeModule.h"
#include "AssetDocumentRequestContextRuntime.h"
#include "AssetDocumentRollbackAction.h"
#include "IPythonScriptPlugin.h"
#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

namespace
{
class FNativeModuleTestPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	FName GetPayloadTypeId() const override { return TEXT("host.native.test"); }
	bool IsAlive() const override { return true; }
};

FAssetDocumentContractValidationResult MakeValidContractResult()
{
	FAssetDocumentContractValidationResult Result; Result.bValid = true; return Result;
}

class FNativeModuleTestTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FNativeModuleTestTransaction()
	{
		Token.RequestId = FGuid::NewGuid();
		Token.TransactionId = FGuid::NewGuid();
		Token.RuntimeRevision = 11;
		RollbackActionEnlistmentResult.bValid = true;
		PostRollbackFinalizerEnlistmentResult.bValid = true;
	}
	FGuid GetTransactionId() const override { return Token.TransactionId; }
	EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Live; }
	const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint>) override { return MakeValidContractResult(); }
	FAssetDocumentContractValidationResult MarkReadyToCommit() override { return MakeValidContractResult(); }
	FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return MakeValidContractResult(); }
	FAssetDocumentContractValidationResult Rollback(FStringView) override { return MakeValidContractResult(); }
	FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		EnlistedRollbackActionComponentIds.Add(
			FString(Action->GetRollbackComponentId()));
		return RollbackActionEnlistmentResult;
	}
	FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return PostRollbackFinalizerEnlistmentResult;
	}
	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;
private:
	FAssetDocumentMutationToken Token;
};

class FNativeModuleTestProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	explicit FNativeModuleTestProvider(TArray<FAssetDocumentPrimitiveDescriptor> InDescriptors)
		: Descriptors(MoveTemp(InDescriptors))
	{
	}

	FString GetProviderId() const override { return TEXT("editor"); }
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override
	{
		return Descriptors;
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
		IAssetDocumentAtomicTransaction* ActiveTransaction) override;

	bool bProbeGIL = false;
	bool bNativeGILReleased = false;
	bool bWorkerAcquiredGIL = false;
	int32 InputInvokeCount = 0;
	TFunction<FAssetDocumentPrimitiveOutcome()> NestedInvoke;
	bool bReturnInvalidStatus = false;
	int32 DiagnosticCount = 0;
	int32 ChangedCount = 0;
	int32 ReturnedHandleCount = 4096;

private:
	FAssetDocumentPrimitiveOutcome InvokeIssue(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles);
	FAssetDocumentPrimitiveOutcome InvokeMany(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles);
	FAssetDocumentPrimitiveOutcome InvokeOther(const FAssetDocumentPrimitiveInvocation& Invocation);
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
};

FAssetDocumentRuntimeValueDto MakeObjectValue()
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	return Value;
}

FAssetDocumentPrimitiveDescriptor MakeIssueDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeAssetDocumentHostTestDescriptor(
		TEXT("editor.host_test.issue"), 1, TEXT("host-test-issue-v1"));
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Object};
	FAssetDocumentPrimitiveHandleResultSchema& Schema =
		Descriptor.HandleResultSchemas.AddDefaulted_GetRef();
	Schema.Name = TEXT("target");
	Schema.Kind = EAssetDocumentHandleKind::Object;
	Schema.GrantedAccess = EAssetDocumentHandleAccess::Mutable;
	Schema.Cardinality = EAssetDocumentHandleResultCardinality::ExactlyOne;
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeInputDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeAssetDocumentHostTestDescriptor(
		TEXT("editor.host_test.input"), 1, TEXT("host-test-input-v1"));
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Object};
	FAssetDocumentPrimitiveHandleArgumentSchema& Schema =
		Descriptor.HandleArgumentSchemas.AddDefaulted_GetRef();
	Schema.Name = TEXT("target");
	Schema.Kind = EAssetDocumentHandleKind::Object;
	Schema.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeNestedDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeAssetDocumentHostTestDescriptor(
		TEXT("editor.host_test.nested"), 1, TEXT("host-test-nested-v1"));
	Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.bRequiresActiveTransaction = true;
	Descriptor.MutationFootprintSchemaId = TEXT("host-test-footprint-v1");
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeManyDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeAssetDocumentHostTestDescriptor(
		TEXT("editor.host_test.many"), 1, TEXT("host-test-many-v1"));
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Object};
	FAssetDocumentPrimitiveHandleResultSchema& Schema = Descriptor.HandleResultSchemas.AddDefaulted_GetRef();
	Schema.Name = TEXT("targets");
	Schema.Kind = EAssetDocumentHandleKind::Object;
	Schema.GrantedAccess = EAssetDocumentHandleAccess::Mutable;
	Schema.Cardinality = EAssetDocumentHandleResultCardinality::ZeroOrMore;
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> MakeDescriptors()
{
	return {
		MakeAssetDocumentHostTestDescriptor(),
		MakeIssueDescriptor(),
		MakeInputDescriptor(),
		MakeManyDescriptor(),
		MakeNestedDescriptor(),
	};
}

FAssetDocumentPrimitiveRegistryBuildInput MakeRegistryInput(
	const TSharedRef<FNativeModuleTestProvider>& Provider)
{
	FAssetDocumentPrimitiveRegistryBuildInput Input;
	Input.RuntimeRevision = 11;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		Input.Required.Add(MakeAssetDocumentHostTestRequirement(Descriptor));
	}
	Input.Providers.Add(Provider);
	return Input;
}

FAssetDocumentPrimitiveOutcome MakeSuccessOutcome(
	const FAssetDocumentRuntimeValueDto& Value)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = Value;
	return Outcome;
}

void AddTransportDiagnostics(
	FAssetDocumentPrimitiveOutcome& Outcome,
	int32 Count)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Outcome.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PrimitiveOutcome");
		Diagnostic.Code = TEXT("AD.RUNTIME.HOST_TEST");
		Diagnostic.Message = TEXT("Host transport diagnostic fixture.");
	}
}

void AddTransportChanges(
	FAssetDocumentPrimitiveOutcome& Outcome,
	int32 Count)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FAssetDocumentChangedRecord& Changed =
			Outcome.ChangedSet.AddDefaulted_GetRef();
		Changed.AuthorityId = TEXT("asset");
		Changed.StableObjectId = FString::FromInt(Index);
		Changed.Path = TEXT("/Body/Value");
		Changed.ChangeKind = TEXT("modified");
	}
}

bool ShouldReturnInvalidStatus(
	bool bEnabled,
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return bEnabled
		&& Invocation.PrimitiveId == TEXT("editor.host_test.echo");
}

FAssetDocumentPrimitiveOutcome FNativeModuleTestProvider::Invoke(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	(void)ActiveTransaction;
 if (bProbeGIL)
 {
  bNativeGILReleased = PyGILState_Check() == 0;
  auto Worker = Async(EAsyncExecution::Thread, []()
  {
   const auto GIL = PyGILState_Ensure();
   PyGILState_Release(GIL);
   return true;
  });
  bWorkerAcquiredGIL = Worker.WaitFor(FTimespan::FromSeconds(2));
 }
	if (Invocation.PrimitiveId == TEXT("editor.host_test.issue"))
	{
		return InvokeIssue(Invocation, Handles);
	}
	if (Invocation.PrimitiveId == TEXT("editor.host_test.many"))
	{
		return InvokeMany(Invocation, Handles);
	}
	return InvokeOther(Invocation);
}

FAssetDocumentPrimitiveOutcome FNativeModuleTestProvider::InvokeOther(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	if (ShouldReturnInvalidStatus(bReturnInvalidStatus, Invocation))
	{
		FAssetDocumentPrimitiveOutcome Invalid = MakeSuccessOutcome(Invocation.ValueArguments);
		Invalid.Status = static_cast<EAssetDocumentPrimitiveOutcomeStatus>(255);
		return Invalid;
	}
	if (Invocation.PrimitiveId == TEXT("editor.host_test.input"))
	{
		++InputInvokeCount;
	}
	if (Invocation.PrimitiveId == TEXT("editor.host_test.nested") && NestedInvoke)
	{
		return NestedInvoke();
	}
	FAssetDocumentPrimitiveOutcome Outcome = MakeSuccessOutcome(Invocation.ValueArguments);
	AddTransportDiagnostics(Outcome, DiagnosticCount);
	AddTransportChanges(Outcome, ChangedCount);
	return Outcome;
}

FAssetDocumentPrimitiveOutcome FNativeModuleTestProvider::InvokeMany(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentPrimitiveOutcome Outcome = MakeSuccessOutcome(Invocation.ValueArguments);
	FAssetDocumentPrimitiveHandleResult& Result = Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("targets");
	for (int32 Index = 0; Index < ReturnedHandleCount; ++Index)
	{
		FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
			EAssetDocumentHandleKind::Object, EAssetDocumentHandleAccess::Mutable,
			MakeUnique<FNativeModuleTestPayload>());
		if (Issued.Handle.IsSet())
		{
			Result.Handles.Add(Issued.Handle.GetValue());
		}
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome FNativeModuleTestProvider::InvokeIssue(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentPrimitiveOutcome Outcome = MakeSuccessOutcome(Invocation.ValueArguments);
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Object,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FNativeModuleTestPayload>());
	if (Issued.Handle.IsSet())
	{
		FAssetDocumentPrimitiveHandleResult& Result = Outcome.HandleResults.AddDefaulted_GetRef();
		Result.Name = TEXT("target");
		Result.Handles.Add(Issued.Handle.GetValue());
	}
	return Outcome;
}

FString TakePythonError()
{
	PyObject* Type = nullptr;
	PyObject* Value = nullptr;
	PyObject* Traceback = nullptr;
	PyErr_Fetch(&Type, &Value, &Traceback);
	PyErr_NormalizeException(&Type, &Value, &Traceback);
	PyObject* Text = Value != nullptr ? PyObject_Str(Value) : nullptr;
	const char* Utf8 = Text != nullptr ? PyUnicode_AsUTF8(Text) : nullptr;
	const FString Result = Utf8 != nullptr ? UTF8_TO_TCHAR(Utf8) : TEXT("");
	Py_XDECREF(Text);
	Py_XDECREF(Type);
	Py_XDECREF(Value);
	Py_XDECREF(Traceback);
	return Result;
}

bool TestHasExactKeys(PyObject* Dictionary, TConstArrayView<const char*> Keys)
{
	if (!PyDict_CheckExact(Dictionary) || PyDict_Size(Dictionary) != Keys.Num())
	{
		return false;
	}
	for (const char* Key : Keys)
	{
		if (PyDict_GetItemString(Dictionary, Key) == nullptr)
		{
			return false;
		}
	}
	return true;
}

void AddPublicModuleKey(TSet<FString>& PublicKeys, PyObject* Key)
{
	const char* Utf8 = PyUnicode_Check(Key) ? PyUnicode_AsUTF8(Key) : nullptr;
	if (Utf8 == nullptr)
	{
		return;
	}
	const FString Name(UTF8_TO_TCHAR(Utf8));
	if (!Name.StartsWith(TEXT("__")))
	{
		PublicKeys.Add(Name);
	}
}

bool HasExpectedModuleSurface(const TSet<FString>& PublicKeys)
{
	return PublicKeys.Num() == 2
		&& PublicKeys.Contains(TEXT("RequestContext"))
		&& PublicKeys.Contains(TEXT("OpaqueHandle"));
}

bool ModuleHasExactSurface(PyObject* Module)
{
	PyObject* Dictionary = PyModule_GetDict(Module);
	TSet<FString> PublicKeys;
	PyObject* Key = nullptr;
	PyObject* Value = nullptr;
	Py_ssize_t Position = 0;
	while (PyDict_Next(Dictionary, &Position, &Key, &Value))
	{
		AddPublicModuleKey(PublicKeys, Key);
	}
	return HasExpectedModuleSurface(PublicKeys);
}

TSet<FString> GetPublicMembers(PyObject* Object)
{
	TSet<FString> Result;
	PyObject* Names = PyObject_Dir(Object);
	for (Py_ssize_t Index = 0; Names != nullptr && Index < PyList_Size(Names); ++Index)
	{
		AddPublicModuleKey(Result, PyList_GetItem(Names, Index));
	}
	Py_XDECREF(Names);
	return Result;
}

void TestPickleRejected(FAutomationTestBase& Test, const TCHAR* Label, PyObject* Object)
{
	PyObject* Pickle = PyImport_ImportModule("pickle");
	PyObject* Dumps = Pickle != nullptr ? PyObject_GetAttrString(Pickle, "dumps") : nullptr;
	PyObject* Result = Dumps != nullptr ? PyObject_CallFunctionObjArgs(Dumps, Object, nullptr) : nullptr;
	Test.TestNull(Label, Result);
	Py_XDECREF(Result); Py_XDECREF(Dumps); Py_XDECREF(Pickle); TakePythonError();
}

void TestProxyTypeHardening(FAutomationTestBase& Test, PyObject* Type, PyObject* Object, const TCHAR* Label)
{
	PyObject* Constructed = PyObject_CallNoArgs(Type);
	Test.TestNull(Label, Constructed); Py_XDECREF(Constructed); TakePythonError();
	Test.TestEqual(TEXT("native proxy rejects attribute assignment"), PyObject_SetAttrString(Object, "pointer", Py_True), -1);
	TakePythonError();
	PyObject* Reduced = PyObject_CallMethod(Object, "__reduce__", nullptr);
	Test.TestNull(TEXT("native proxy rejects reduce"), Reduced); Py_XDECREF(Reduced); TakePythonError();
	TestPickleRejected(Test, TEXT("native proxy rejects pickle.dumps"), Object);
}

bool HasRequiredContextMembers(const TSet<FString>& Members)
{
	return Members.Contains(TEXT("handoff")) && Members.Contains(TEXT("invoke")) && Members.Contains(TEXT("inspect_catalog")) && Members.Contains(TEXT("request_id"))
		&& Members.Contains(TEXT("runtime_revision")) && Members.Contains(TEXT("phase"));
}

bool HasExactContextMembers(const TSet<FString>& Members)
{
	return Members.Num() == 6 && HasRequiredContextMembers(Members);
}

PyObject* CallInvoke(
	PyObject* Context,
	const char* PrimitiveId,
	PyObject* ValueArguments,
	PyObject* HandleArguments)
{
	PyObject* Method = PyObject_GetAttrString(Context, "invoke");
	PyObject* Arguments = PyTuple_New(4);
	PyTuple_SetItem(Arguments, 0, PyUnicode_FromString(PrimitiveId));
	PyTuple_SetItem(Arguments, 1, PyLong_FromLong(1));
	Py_INCREF(ValueArguments);
	PyTuple_SetItem(Arguments, 2, ValueArguments);
	Py_INCREF(HandleArguments);
	PyTuple_SetItem(Arguments, 3, HandleArguments);
	PyObject* Result = PyObject_Call(Method, Arguments, nullptr);
	Py_DECREF(Arguments);
	Py_DECREF(Method);
	return Result;
}

void TestTypeHardening(
	FAutomationTestBase& Test,
	PyObject* Module,
	PyObject* Context,
	PyObject* Handle)
{
	Test.TestTrue(TEXT("module surface is exact"), ModuleHasExactSurface(Module));
	PyObject* ContextType = PyObject_GetAttrString(Module, "RequestContext");
	PyObject* HandleType = PyObject_GetAttrString(Module, "OpaqueHandle");
	TestProxyTypeHardening(Test, ContextType, Context, TEXT("RequestContext rejects construction"));
	TestProxyTypeHardening(Test, HandleType, Handle, TEXT("OpaqueHandle rejects construction"));
	const TSet<FString> ContextMembers = GetPublicMembers(Context);
	const TSet<FString> HandleMembers = GetPublicMembers(Handle);
	Test.TestTrue(TEXT("RequestContext members are exact"), HasExactContextMembers(ContextMembers));
	Test.TestTrue(TEXT("OpaqueHandle exposes no public metadata"), HandleMembers.IsEmpty());
	Py_DECREF(HandleType); Py_DECREF(ContextType);
}

PyObject* TestIssueOutcome(
	FAutomationTestBase& Test,
	PyObject* Context)
{
	PyObject* Value = PyDict_New();
	PyObject* Handles = PyTuple_New(0);
	PyObject* Outcome = CallInvoke(Context, "editor.host_test.issue", Value, Handles);
	Py_DECREF(Handles);
	Py_DECREF(Value);
	static constexpr const char* OutcomeKeys[] = {
		"status", "value", "handle_results", "changed_set", "diagnostics"};
	Test.TestTrue(TEXT("outcome has exact five keys"), TestHasExactKeys(Outcome, OutcomeKeys));
	PyObject* Groups = PyDict_GetItemString(Outcome, "handle_results");
	Test.TestTrue(TEXT("handle results are an immutable tuple"), PyTuple_CheckExact(Groups) != 0);
	PyObject* Group = PyTuple_GetItem(Groups, 0);
	static constexpr const char* GroupKeys[] = {"name", "handles"};
	Test.TestTrue(TEXT("handle group keys are exact"), TestHasExactKeys(Group, GroupKeys));
	PyObject* HandleProxies = PyDict_GetItemString(Group, "handles");
	PyObject* Proxy = PyTuple_GetItem(HandleProxies, 0);
	Py_INCREF(Proxy);
	PyObject* Repr = PyObject_Repr(Proxy);
	Test.TestEqual(TEXT("handle repr is redacted"),
		FString(UTF8_TO_TCHAR(PyUnicode_AsUTF8(Repr))),
		FString(TEXT("<_assetdocument_native.OpaqueHandle redacted>")));
	Py_DECREF(Repr);
	Py_DECREF(Outcome);
	return Proxy;
}

bool CatalogDescriptorHasExactShape(PyObject* Descriptor)
{
	static constexpr const char* Keys[] = {"primitive_id", "version", "provider_id", "owner", "execution_thread", "access",
		"allowed_phases", "allowed_handle_kinds", "handle_argument_schemas", "handle_result_schemas", "requires_active_transaction",
		"input_schema_id", "output_schema_id", "mutation_footprint_schema_id", "stable_diagnostic_codes", "required_consumer_ids", "admission"};
	return TestHasExactKeys(Descriptor, Keys) && PyLong_CheckExact(PyDict_GetItemString(Descriptor, "owner"))
		&& PyTuple_CheckExact(PyDict_GetItemString(Descriptor, "allowed_phases"))
		&& PyTuple_CheckExact(PyDict_GetItemString(Descriptor, "handle_argument_schemas"))
		&& PyTuple_CheckExact(PyDict_GetItemString(Descriptor, "handle_result_schemas"));
}

bool CatalogAdmissionHasExactShape(PyObject* Admission)
{
	static constexpr const char* Keys[] = {"contract_version", "disposition", "authority_path", "exact_engine_version", "physical_owner",
		"public_module", "public_header", "exported_authoritative_api", "authority_context_id", "primary_operation_id", "identity_lifetime_contract_id",
		"transaction_contract_id", "rollback_contract_id", "completion_contract_id", "save_reload_evidence_id", "failure_injection_test_ids",
		"verification_test_ids", "source_marker", "raw_lifecycle_allowlist_id", "governance_id", "independent_candidate_review_id",
		"independent_bridge_review_id", "helper_replacement_condition", "removal_condition", "authority_fingerprint"};
	return TestHasExactKeys(Admission, Keys) && PyLong_CheckExact(PyDict_GetItemString(Admission, "disposition"))
		&& PyTuple_CheckExact(PyDict_GetItemString(Admission, "failure_injection_test_ids"))
		&& PyTuple_CheckExact(PyDict_GetItemString(Admission, "verification_test_ids"));
}

bool CatalogSchemasHaveExactShape(PyObject* Descriptor)
{
	static constexpr const char* ArgumentKeys[] = {"name", "kind", "required_access", "required"};
	static constexpr const char* ResultKeys[] = {"name", "kind", "granted_access", "cardinality"};
	PyObject* Arguments = PyDict_GetItemString(Descriptor, "handle_argument_schemas");
	PyObject* Results = PyDict_GetItemString(Descriptor, "handle_result_schemas");
	const bool bArgumentValid = PyTuple_Size(Arguments) == 0 || TestHasExactKeys(PyTuple_GetItem(Arguments, 0), ArgumentKeys);
	const bool bResultValid = PyTuple_Size(Results) == 0 || TestHasExactKeys(PyTuple_GetItem(Results, 0), ResultKeys);
	return bArgumentValid && bResultValid;
}

bool CatalogIsSorted(PyObject* Descriptors)
{
	FString Previous;
	for (Py_ssize_t Index = 0; Index < PyTuple_Size(Descriptors); ++Index)
	{
		const char* Utf8 = PyUnicode_AsUTF8(PyDict_GetItemString(PyTuple_GetItem(Descriptors, Index), "primitive_id"));
		const FString Current = UTF8_TO_TCHAR(Utf8);
		if (!Previous.IsEmpty() && Previous > Current)
		{
			return false;
		}
		Previous = Current;
	}
	return true;
}

bool IsExactCatalogDescriptor(PyObject* Descriptor)
{
	return CatalogDescriptorHasExactShape(Descriptor)
		&& CatalogAdmissionHasExactShape(PyDict_GetItemString(Descriptor, "admission"))
		&& CatalogSchemasHaveExactShape(Descriptor);
}

bool HasExactlyOneResultSchema(PyObject* Descriptor)
{
	PyObject* Results = PyDict_GetItemString(Descriptor, "handle_result_schemas");
	if (PyTuple_Size(Results) == 0)
	{
		return false;
	}
	PyObject* Cardinality = PyDict_GetItemString(PyTuple_GetItem(Results, 0), "cardinality");
	return PyLong_CheckExact(Cardinality) && PyLong_AsLong(Cardinality) == 0;
}

bool CatalogDescriptorsAreExact(PyObject* Descriptors)
{
	bool bSawExactlyOne = false;
	for (Py_ssize_t Index = 0; Index < PyTuple_Size(Descriptors); ++Index)
	{
		PyObject* Descriptor = PyTuple_GetItem(Descriptors, Index);
		if (!IsExactCatalogDescriptor(Descriptor))
		{
			return false;
		}
		if (HasExactlyOneResultSchema(Descriptor))
		{
			bSawExactlyOne = true;
		}
	}
	return bSawExactlyOne;
}

void TestCatalog(FAutomationTestBase& Test, PyObject* Context)
{
	PyObject* Catalog = PyObject_CallMethod(Context, "inspect_catalog", nullptr);
	static constexpr const char* CatalogKeys[] = {
		"runtime_revision", "frozen", "descriptors", "providers"};
	Test.TestTrue(TEXT("catalog top-level keys are exact"), TestHasExactKeys(Catalog, CatalogKeys));
	PyObject* Descriptors = PyDict_GetItemString(Catalog, "descriptors");
	PyObject* Providers = PyDict_GetItemString(Catalog, "providers");
	Test.TestTrue(TEXT("catalog descriptors are a nonempty tuple"),
		PyTuple_CheckExact(Descriptors) && PyTuple_Size(Descriptors) > 0);
	Test.TestTrue(TEXT("catalog providers are a nonempty tuple"),
		PyTuple_CheckExact(Providers) && PyTuple_Size(Providers) > 0);
	Test.TestTrue(TEXT("all descriptor, admission and schema projections are exact"), CatalogDescriptorsAreExact(Descriptors));
	static constexpr const char* ProviderKeys[] = {"provider_id", "state", "diagnostic"};
	Test.TestTrue(TEXT("provider key set is exact"), TestHasExactKeys(PyTuple_GetItem(Providers, 0), ProviderKeys));
	Test.TestTrue(TEXT("catalog descriptor ordering is stable"), CatalogIsSorted(Descriptors));
	Py_DECREF(Catalog);
}

FAssetDocumentRuntimeValueDto MakeDepthValue(int32 Depth)
{
	FAssetDocumentRuntimeValueDto Root;
	Root.Kind = EAssetDocumentRuntimeValueKind::Array;
	FAssetDocumentRuntimeValueDto* Current = &Root;
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		Current = &Current->Elements.AddDefaulted_GetRef();
		Current->Kind = EAssetDocumentRuntimeValueKind::Array;
	}
	return Root;
}

FAssetDocumentRuntimeValueDto MakeNodeValue(int32 Nodes)
{
	FAssetDocumentRuntimeValueDto Root;
	Root.Kind = EAssetDocumentRuntimeValueKind::Array;
	Root.Elements.SetNum(Nodes - 1);
	return Root;
}

void TestValueBounds(FAutomationTestBase& Test)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	FAssetDocumentRuntimeValueDto Decoded;
	PyObject* Depth64 = AssetDocumentPythonEncodeValue(MakeDepthValue(64));
	Test.TestNotNull(TEXT("depth 64 encodes"), Depth64);
	Test.TestTrue(TEXT("depth 64 round-trips"), AssetDocumentPythonDecodeValue(Depth64, Decoded, Diagnostic));
	Py_XDECREF(Depth64);
	PyObject* Depth65 = AssetDocumentPythonEncodeValue(MakeDepthValue(65));
	Test.TestNull(TEXT("depth 65 fails"), Depth65);
	TakePythonError();
	PyObject* Nodes4096 = AssetDocumentPythonEncodeValue(MakeNodeValue(4096));
	Test.TestNotNull(TEXT("4096 nodes encode"), Nodes4096);
	Test.TestTrue(TEXT("4096 nodes round-trip"), AssetDocumentPythonDecodeValue(Nodes4096, Decoded, Diagnostic));
	Py_XDECREF(Nodes4096);
	PyObject* Nodes4097 = AssetDocumentPythonEncodeValue(MakeNodeValue(4097));
	Test.TestNull(TEXT("4097 nodes fail"), Nodes4097);
	TakePythonError();
	FAssetDocumentRuntimeRequestDto ProjectionRequest;
	ProjectionRequest.RequestId = FGuid::NewGuid(); ProjectionRequest.Operation = EAssetDocumentRuntimeOperation::Inspect;
	ProjectionRequest.TargetAssetPath = TEXT("/Game/AssetDocument/T_HostProjection.T_HostProjection"); ProjectionRequest.ExpectedRuntimeRevision = 71;
	const FString ProjectionRequestId = ProjectionRequest.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower);
	PyObject* ProjectionObject = Py_BuildValue(
		"{s:i,s:s,s:i,s:i,s:K,s:({s:s,s:s,s:s,s:i,s:({s:s,s:s})}),s:s,s:y,s:s,s:i,s:({s:s,s:s,s:s,s:s})}",
		"abi_version", 1, "request_id", TCHAR_TO_UTF8(*ProjectionRequestId), "terminal_status", 0, "final_phase", 6,
		"runtime_revision", static_cast<unsigned long long>(71), "diagnostics", "path", "/Body/Value", "code", "Runtime.Test",
		"message", "fixture-only field projection diagnostic", "severity", 1, "context", "key", "actual", "value", "diagnostic-marker",
		"canonical_value", "field-projection-value", "canonical_bytes", "fixture-only-transport-marker:revision-one",
		"semantic_hash", "revision-one", "codec_version", 1, "changed_set", "authority_id", "asset",
		"stable_object_id", "object", "path", "/Body/Value", "change_kind", "modified");
	FAssetDocumentRuntimeResultDto ProjectionResult; FAssetDocumentRuntimeDiagnostic ProjectionFailure;
	Test.TestTrue(TEXT("exact eleven-key result decodes"), AssetDocumentPythonDecodeResult(ProjectionObject, ProjectionResult, ProjectionFailure));
	Test.TestEqual(TEXT("abi version projects"), ProjectionResult.AbiVersion, uint32{1}); Test.TestEqual(TEXT("request id projects"), ProjectionResult.RequestId, ProjectionRequest.RequestId);
	Test.TestEqual(TEXT("terminal status projects"), static_cast<uint8>(ProjectionResult.TerminalStatus), uint8{0}); Test.TestEqual(TEXT("final phase projects"), static_cast<uint8>(ProjectionResult.FinalPhase), uint8{6}); Test.TestEqual(TEXT("runtime revision projects"), ProjectionResult.RuntimeRevision, uint64{71});
	Test.TestEqual(TEXT("one diagnostic projects"), ProjectionResult.Diagnostics.Num(), 1);
	if (ProjectionResult.Diagnostics.Num() != 1) { Py_DECREF(ProjectionObject); return; }
	const FAssetDocumentRuntimeDiagnostic& ProjectedDiagnostic = ProjectionResult.Diagnostics[0];
	Test.TestEqual(TEXT("diagnostic path projects"), ProjectedDiagnostic.Path, FString(TEXT("/Body/Value"))); Test.TestEqual(TEXT("diagnostic code projects"), ProjectedDiagnostic.Code, FString(TEXT("Runtime.Test")));
	Test.TestEqual(TEXT("diagnostic message projects"), ProjectedDiagnostic.Message, FString(TEXT("fixture-only field projection diagnostic"))); Test.TestEqual(TEXT("diagnostic severity projects"), static_cast<uint8>(ProjectedDiagnostic.Severity), uint8{1});
	const bool bProjectedNestedShape = (ProjectedDiagnostic.Context.Num() == 1) & (ProjectionResult.ChangedSet.Num() == 1);
	Test.TestTrue(TEXT("diagnostic context and changed record project"), bProjectedNestedShape);
	if (!bProjectedNestedShape) { Py_DECREF(ProjectionObject); return; }
	Test.TestEqual(TEXT("diagnostic context key projects"), ProjectedDiagnostic.Context[0].Key, FString(TEXT("actual"))); Test.TestEqual(TEXT("diagnostic context value projects"), ProjectedDiagnostic.Context[0].Value.StringValue, FString(TEXT("diagnostic-marker")));
	Test.TestTrue(TEXT("canonical value is present"), ProjectionResult.CanonicalValue.IsSet()); Test.TestEqual(TEXT("canonical value projects"), ProjectionResult.CanonicalValue->StringValue, FString(TEXT("field-projection-value")));
	const ANSICHAR* Marker = "fixture-only-transport-marker:revision-one"; TArray<uint8> ExpectedMarker; ExpectedMarker.Append(reinterpret_cast<const uint8*>(Marker), FCStringAnsi::Strlen(Marker));
	Test.TestTrue(TEXT("fixture-only transport marker projects independently"), ProjectionResult.CanonicalBytes == ExpectedMarker);
	Test.TestEqual(TEXT("semantic hash projects independently"), ProjectionResult.SemanticHash, FString(TEXT("revision-one"))); Test.TestEqual(TEXT("codec version projects"), ProjectionResult.CodecVersion, uint32{1});
	const FAssetDocumentChangedRecord& ProjectedChange = ProjectionResult.ChangedSet[0];
	Test.TestEqual(TEXT("changed authority projects"), ProjectedChange.AuthorityId, FString(TEXT("asset"))); Test.TestEqual(TEXT("changed object projects"), ProjectedChange.StableObjectId, FString(TEXT("object")));
	Test.TestEqual(TEXT("changed path projects"), ProjectedChange.Path, FString(TEXT("/Body/Value"))); Test.TestEqual(TEXT("changed kind projects"), ProjectedChange.ChangeKind, FString(TEXT("modified")));
	Test.TestTrue(TEXT("fixture-only projection satisfies only the frozen tuple shape"), ValidateAssetDocumentRuntimeResult(ProjectionRequest, ProjectionResult).bValid);
	// The marker is fixture-only transport data: these assertions claim field fidelity and tuple shape, not encoding, hashing, or a production DocumentCodec relationship.
	Py_DECREF(ProjectionObject);
}

FString GetOutcomeDiagnosticCode(PyObject* Outcome);

void TestReturnedHandleBounds(
	FAutomationTestBase& Test,
	PyObject* Context,
	FNativeModuleTestProvider& Provider)
{
	PyObject* Value = PyDict_New(); PyObject* Handles = PyTuple_New(0);
	PyObject* Outcome = CallInvoke(Context, "editor.host_test.many", Value, Handles);
	PyObject* Groups = Outcome != nullptr ? PyDict_GetItemString(Outcome, "handle_results") : nullptr;
	PyObject* Proxies = Groups != nullptr ? PyDict_GetItemString(PyTuple_GetItem(Groups, 0), "handles") : nullptr;
	Test.TestTrue(TEXT("4096 resolver-issued handles encode"), Proxies != nullptr && PyTuple_Size(Proxies) == 4096);
	Py_XDECREF(Outcome);
	Provider.ReturnedHandleCount = 4097;
	PyObject* TooMany = CallInvoke(Context, "editor.host_test.many", Value, Handles);
	Test.TestNotNull(TEXT("4097 returned handles produce a typed failed outcome"), TooMany);
	Test.TestEqual(TEXT("4097 returned handles preserve the diagnostic code"),
		GetOutcomeDiagnosticCode(TooMany),
		FString(TEXT("PrimitiveOutcome.HandleResultLimitExceeded")));
	Provider.ReturnedHandleCount = 4096;
	Py_XDECREF(TooMany); Py_DECREF(Handles); Py_DECREF(Value);
}

void ExpectInvokeError(FAutomationTestBase& Test, const TCHAR* Label, PyObject* Context,
	const char* PrimitiveId, long Version, PyObject* Value, PyObject* Handles)
{
	PyObject* Method = PyObject_GetAttrString(Context, "invoke");
	PyObject* Args = PyTuple_Pack(4, PyUnicode_FromString(PrimitiveId), PyLong_FromLong(Version), Value, Handles);
	PyObject* Result = PyObject_Call(Method, Args, nullptr);
	Test.TestNull(Label, Result); Py_XDECREF(Result); Py_DECREF(Args); Py_DECREF(Method); TakePythonError();
}

FString InvokeAndTakeError(
	PyObject* Context,
	const char* PrimitiveId,
	long Version,
	PyObject* Value,
	PyObject* Handles)
{
	PyObject* Method = PyObject_GetAttrString(Context, "invoke");
	PyObject* Args = PyTuple_Pack(
		4, PyUnicode_FromString(PrimitiveId), PyLong_FromLong(Version), Value, Handles);
	PyObject* Result = PyObject_Call(Method, Args, nullptr);
	Py_XDECREF(Result);
	Py_DECREF(Args);
	Py_DECREF(Method);
	return TakePythonError();
}

PyObject* GetFirstOutcomeDiagnostic(PyObject* Outcome)
{
	if (Outcome == nullptr) { return nullptr; }
	PyObject* Diagnostics = PyDict_GetItemString(Outcome, "diagnostics");
	if (Diagnostics == nullptr || !PyTuple_CheckExact(Diagnostics)
		|| PyTuple_Size(Diagnostics) == 0) { return nullptr; }
	return PyTuple_GetItem(Diagnostics, 0);
}

FString GetOutcomeDiagnosticCode(PyObject* Outcome)
{
	PyObject* Diagnostic = GetFirstOutcomeDiagnostic(Outcome);
	if (Diagnostic == nullptr) { return {}; }
	PyObject* Code = PyDict_GetItemString(Diagnostic, "code");
	if (Code == nullptr || !PyUnicode_CheckExact(Code)) { return {}; }
	const char* Utf8 = PyUnicode_AsUTF8(Code);
	if (Utf8 == nullptr) { return {}; }
	return UTF8_TO_TCHAR(Utf8);
}

Py_ssize_t GetOutcomeTupleSize(
	PyObject* Outcome,
	const char* Field)
{
	PyObject* Tuple = Outcome != nullptr
		? PyDict_GetItemString(Outcome, Field)
		: nullptr;
	return Tuple != nullptr && PyTuple_CheckExact(Tuple)
		? PyTuple_Size(Tuple)
		: -1;
}

void TestOutcomeTransportLimits(
	FAutomationTestBase& Test,
	PyObject* Context,
	FNativeModuleTestProvider& Provider)
{
	PyObject* Value = PyDict_New();
	PyObject* Handles = PyTuple_New(0);
	Provider.DiagnosticCount = 256;
	PyObject* AcceptedDiagnostics =
		CallInvoke(Context, "editor.host_test.echo", Value, Handles);
	Test.TestEqual(TEXT("256 diagnostics are accepted"),
		GetOutcomeTupleSize(AcceptedDiagnostics, "diagnostics"), Py_ssize_t{256});
	Py_XDECREF(AcceptedDiagnostics);
	Provider.DiagnosticCount = 257;
	PyObject* RejectedDiagnostics =
		CallInvoke(Context, "editor.host_test.echo", Value, Handles);
	Test.TestNull(TEXT("257 diagnostics are rejected"), RejectedDiagnostics);
	Test.TestTrue(TEXT("diagnostic overflow uses Host transport code"),
		TakePythonError().Contains(TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED")));
	Py_XDECREF(RejectedDiagnostics);
	Provider.DiagnosticCount = 0;
	Provider.ChangedCount = 4096;
	PyObject* AcceptedChanges =
		CallInvoke(Context, "editor.host_test.echo", Value, Handles);
	Test.TestEqual(TEXT("4096 changed records are accepted"),
		GetOutcomeTupleSize(AcceptedChanges, "changed_set"), Py_ssize_t{4096});
	Py_XDECREF(AcceptedChanges);
	Provider.ChangedCount = 4097;
	PyObject* RejectedChanges =
		CallInvoke(Context, "editor.host_test.echo", Value, Handles);
	Test.TestNull(TEXT("4097 changed records are rejected"), RejectedChanges);
	Test.TestTrue(TEXT("changed overflow uses Host transport code"),
		TakePythonError().Contains(TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED")));
	Py_XDECREF(RejectedChanges);
	Provider.ChangedCount = 0;
	Py_DECREF(Handles);
	Py_DECREF(Value);
}

void TestDiagnosticFidelity(
	FAutomationTestBase& Test,
	PyObject* Context,
	FNativeModuleTestProvider& Provider)
{
	PyObject* Value = PyDict_New();
	PyObject* Handles = PyTuple_New(0);
	const FString VersionError = InvokeAndTakeError(
		Context, "editor.host_test.echo", 2, Value, Handles);
	Test.TestTrue(TEXT("wrong version preserves registry diagnostic"),
		VersionError.Contains(TEXT("AD.RUNTIME.PRIMITIVE_VERSION_MISMATCH")));
	Provider.bReturnInvalidStatus = true;
	PyObject* InvalidOutcome = CallInvoke(
		Context, "editor.host_test.echo", Value, Handles);
	Provider.bReturnInvalidStatus = false;
	Test.TestNotNull(TEXT("invalid provider outcome is projected as failed"), InvalidOutcome);
	Test.TestEqual(TEXT("invalid provider outcome preserves its diagnostic"),
		GetOutcomeDiagnosticCode(InvalidOutcome),
		FString(TEXT("PrimitiveOutcome.InvalidStatus")));
	Py_XDECREF(InvalidOutcome);
	Py_DECREF(Handles);
	Py_DECREF(Value);
}

void TestFailureClosedInputs(FAutomationTestBase& Test, PyObject* Context, PyObject* Proxy,
	FAssetDocumentNativeRequestContextRuntime& Runtime)
{
	PyObject* ObjectValue = PyDict_New(); PyObject* EmptyHandles = PyTuple_New(0); PyObject* WrongValue = PyTuple_New(0);
	ExpectInvokeError(Test, TEXT("wrong primitive version fails closed"), Context, "editor.host_test.echo", 2, ObjectValue, EmptyHandles);
	ExpectInvokeError(Test, TEXT("wrong value container fails closed"), Context, "editor.host_test.echo", 1, WrongValue, EmptyHandles);

	PyObject* Named = PyDict_New(); PyDict_SetItemString(Named, "name", PyUnicode_FromString("target"));
	PyDict_SetItemString(Named, "handle", Proxy); PyDict_SetItemString(Named, "extra", Py_True);
	PyObject* BadKeys = PyTuple_Pack(1, Named);
	ExpectInvokeError(Test, TEXT("wrong handle key set fails closed"), Context, "editor.host_test.input", 1, ObjectValue, BadKeys);
	PyDict_DelItemString(Named, "extra"); PyObject* Forged = PyDict_New(); PyDict_SetItemString(Named, "handle", Forged);
	ExpectInvokeError(Test, TEXT("forged handle fails closed"), Context, "editor.host_test.input", 1, ObjectValue, BadKeys);

	TSharedRef<FAssetDocumentPythonDispatchBindingState> OtherBinding = AssetDocumentPythonOpenDispatchBinding(Runtime);
	PyObject* WrongBindingProxy = AssetDocumentPythonCreateOpaqueHandleProxy(OtherBinding->BindingId, FAssetDocumentOpaqueHandle());
	PyDict_SetItemString(Named, "handle", WrongBindingProxy);
	ExpectInvokeError(Test, TEXT("wrong proxy binding fails closed"), Context, "editor.host_test.input", 1, ObjectValue, BadKeys);
	AssetDocumentPythonCloseDispatchBinding(OtherBinding);
	Py_DECREF(WrongBindingProxy); Py_DECREF(Forged); Py_DECREF(BadKeys); Py_DECREF(Named);
	Py_DECREF(WrongValue); Py_DECREF(EmptyHandles); Py_DECREF(ObjectValue);
}

void TestNestedMutation(FAutomationTestBase& Test, PyObject* Context,
	FNativeModuleTestProvider& Provider, FAssetDocumentNativeRequestContextRuntime& Runtime)
{
	Provider.NestedInvoke = [&Runtime]()
	{
		FAssetDocumentPrimitiveInvocation Nested; Nested.PrimitiveId = TEXT("editor.host_test.nested");
		Nested.Version = 1; Nested.ValueArguments = MakeObjectValue(); return Runtime.Invoke(Nested);
	};
	PyObject* Value = PyDict_New(); PyObject* Handles = PyTuple_New(0);
	PyObject* Outcome = CallInvoke(Context, "editor.host_test.nested", Value, Handles);
	PyObject* Diagnostics = Outcome != nullptr ? PyDict_GetItemString(Outcome, "diagnostics") : nullptr;
	PyObject* Code = Diagnostics != nullptr ? PyDict_GetItemString(PyTuple_GetItem(Diagnostics, 0), "code") : nullptr;
	Test.TestTrue(TEXT("nested mutating invocation has stable rejection"), Code != nullptr
		&& FString(UTF8_TO_TCHAR(PyUnicode_AsUTF8(Code))) == TEXT("AD.RUNTIME.NESTED_MUTATION"));
	Py_XDECREF(Outcome); Py_DECREF(Handles); Py_DECREF(Value); Provider.NestedInvoke = nullptr;
}

void TestStaleAndForgedHandles(
	FAutomationTestBase& Test,
	PyObject* Context,
	PyObject* Proxy,
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& Handles,
	const TSharedRef<FNativeModuleTestProvider>& Provider)
{
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = EAssetDocumentHandleInvalidationReason::GarbageCollection;
	Handles->Invalidate(Event);
	PyObject* Named = PyDict_New();
	PyDict_SetItemString(Named, "name", PyUnicode_FromString("target"));
	PyDict_SetItemString(Named, "handle", Proxy);
	PyObject* HandleArguments = PyTuple_Pack(1, Named);
	PyObject* Value = PyDict_New();
	PyObject* Result = CallInvoke(Context, "editor.host_test.input", Value, HandleArguments);
	Test.TestNotNull(TEXT("stale input returns a typed failed outcome"), Result);
	PyObject* Diagnostics = PyDict_GetItemString(Result, "diagnostics");
	PyObject* Diagnostic = PyTuple_GetItem(Diagnostics, 0);
	PyObject* Code = PyDict_GetItemString(Diagnostic, "code");
	Test.TestEqual(TEXT("stale input reports stable code"),
		FString(UTF8_TO_TCHAR(PyUnicode_AsUTF8(Code))),
		FString(TEXT("AD.RUNTIME.STALE_HANDLE")));
	Test.TestEqual(TEXT("stale input keeps provider call count zero"), Provider->InputInvokeCount, 0);
	Py_DECREF(Result);
	Py_DECREF(Value);
	Py_DECREF(HandleArguments);
	Py_DECREF(Named);
	PyObject* Forged = PyDict_New();
	Test.TestEqual(TEXT("forged dictionaries are not native handles"),
		AssetDocumentPythonIsOpaqueHandleProxy(Forged), false);
	Py_DECREF(Forged);
}

void TestExpiredBinding(
	FAutomationTestBase& Test,
	const TSharedRef<FAssetDocumentPythonDispatchBindingState>& Binding,
	PyObject* Context,
	PyObject* Proxy)
{
	AssetDocumentPythonCloseDispatchBinding(Binding);
	PyObject* Catalog = PyObject_CallMethod(Context, "inspect_catalog", nullptr);
	Test.TestNull(TEXT("retained context fails after binding teardown"), Catalog);
	Test.TestTrue(TEXT("expired context reports stable code"),
		TakePythonError().Contains(TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED")));
	FAssetDocumentOpaqueHandle Handle; FAssetDocumentRuntimeDiagnostic Diagnostic;
	Test.TestFalse(TEXT("retained proxy fails after binding teardown"),
		AssetDocumentPythonResolveOpaqueHandleProxy(Proxy, Binding->BindingId, Handle, Diagnostic));
	Test.TestEqual(TEXT("expired proxy reports stable code"), Diagnostic.Code, FString(TEXT("AD.RUNTIME.STALE_HANDLE")));
}

FAssetDocumentPrimitiveRegistryBuildInput MakeHandoffNativeRegistryInput(
	const TSharedRef<FNativeModuleTestProvider>& EditorProvider)
{
	FAssetDocumentPrimitiveRegistryBuildInput Input =
		MakeRegistryInput(EditorProvider);
	FAssetDocumentPrimitiveDescriptor Animation =
		MakeAssetDocumentHostTestDescriptor(
			TEXT("animation.host_test.consume"),
			1,
			TEXT("host-test-animation-consume-v1"));
	Animation.ProviderId = TEXT("animation");
	Animation.Owner = EAssetDocumentPrimitiveOwner::Animation;
	Animation.Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
	Input.Required.Add(MakeAssetDocumentHostTestRequirement(Animation));
	Input.Providers.Add(MakeShared<FAssetDocumentHostTestProvider>(
		TEXT("animation"),
		TArray<FAssetDocumentPrimitiveDescriptor>{MoveTemp(Animation)}));
	return Input;
}

FAssetDocumentOpaqueHandle IssueNativeHandoffSource(
	FAssetDocumentHandleRegistryRuntime& Handles,
	const FAssetDocumentRequestContextMetadata& Metadata,
	const FGuid& TransactionId,
	EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::Mutable)
{
	FAssetDocumentHandleResolverBinding ResolverBinding;
	ResolverBinding.RequestId = Metadata.RequestId;
	ResolverBinding.TransactionId = TransactionId;
	ResolverBinding.RuntimeRevision = Metadata.PinnedRuntimeRevision;
	ResolverBinding.ProviderId = TEXT("editor");
	ResolverBinding.Phase = Metadata.Phase;
	FAssetDocumentPrimitiveHandleResultSchema& Schema =
		ResolverBinding.AllowedResults.AddDefaulted_GetRef();
	Schema.Name = TEXT("asset");
	Schema.Kind = EAssetDocumentHandleKind::Asset;
	Schema.GrantedAccess = Access;
	Schema.Cardinality = EAssetDocumentHandleResultCardinality::ExactlyOne;
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session =
		Handles.CreateInvocationSession(ResolverBinding);
	FAssetDocumentIssuedHandleResult Issued = Session->Issue(
		EAssetDocumentHandleKind::Asset,
		Access,
		MakeUnique<FNativeModuleTestPayload>());
	check(Issued.Handle.IsSet());
	FAssetDocumentPrimitiveHandleResult Returned;
	Returned.Name = TEXT("asset");
	Returned.Handles.Add(Issued.Handle.GetValue());
	check(Session->MarkValidatedReturnedHandles(
		MakeArrayView(&Returned, 1)).bValid);
	Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Succeeded);
	return Issued.Handle.GetValue();
}

PyObject* CallHandoff(
	PyObject* Context,
	PyObject* Handle,
	PyObject* TargetProvider,
	PyObject* Access)
{
	PyObject* Method = PyObject_GetAttrString(Context, "handoff");
	if (Method == nullptr)
	{
		return nullptr;
	}
	PyObject* Arguments = PyTuple_Pack(3, Handle, TargetProvider, Access);
	PyObject* Result = PyObject_Call(Method, Arguments, nullptr);
	Py_DECREF(Arguments);
	Py_DECREF(Method);
	return Result;
}

FString CallHandoffAndTakeError(
	PyObject* Context,
	PyObject* Handle,
	const char* TargetProvider,
	const char* Access)
{
	PyObject* Target = PyUnicode_FromString(TargetProvider);
	PyObject* AccessValue = PyUnicode_FromString(Access);
	PyObject* Result = CallHandoff(Context, Handle, Target, AccessValue);
	Py_XDECREF(Result);
	Py_DECREF(AccessValue);
	Py_DECREF(Target);
	return TakePythonError();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPythonHostRejectsArbitraryExecuteTest,
	"UEAssetDoc.PythonHost.RejectsArbitraryExecute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPythonHostRejectsArbitraryExecuteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	TestNotNull(TEXT("Engine Python plugin is loaded"), Python);
	TestTrue(TEXT("Engine Python is initialized"), Python != nullptr && Python->IsPythonInitialized());
	if (Python == nullptr || !Python->IsPythonInitialized())
	{
		return false;
	}
	PyGILState_STATE Gil = PyGILState_Ensure();
	const FAssetDocumentContractValidationResult Install = AssetDocumentPythonInstallNativeModule();
	TestTrue(TEXT("typed native module installs"), Install.bValid);
	PyObject* Module = PyImport_ImportModule("_assetdocument_native");
	TestNotNull(TEXT("typed native module imports"), Module);
	TSharedRef<FNativeModuleTestProvider> Provider = MakeShared<FNativeModuleTestProvider>(MakeDescriptors());
	const FAssetDocumentPrimitiveRegistryBuildResult Build =
		FAssetDocumentPrimitiveRegistryRuntime::CreateFrozen(MakeRegistryInput(Provider));
	TestTrue(TEXT("native test catalog freezes"), Build.Registry.IsValid());
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Handles = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = FGuid::NewGuid();
	Metadata.PinnedRuntimeRevision = 11;
	Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	FNativeModuleTestTransaction Transaction;
	FAssetDocumentNativeRequestContextRuntime ContextRuntime(
		Metadata, Transaction.GetTransactionId(), Build.Registry.ToSharedRef(), Handles, &Transaction);
	TSharedRef<FAssetDocumentPythonDispatchBindingState> Binding =
		AssetDocumentPythonOpenDispatchBinding(ContextRuntime);
	PyObject* Context = AssetDocumentPythonCreateRequestContextProxy(Binding);
	Provider->bProbeGIL = true;
	PyObject* Proxy = TestIssueOutcome(*this, Context);
 TestTrue(TEXT("pure native invocation releases GIL"), Provider->bNativeGILReleased);
 TestTrue(TEXT("reference-scanner-like worker acquires GIL within bounded native call"), Provider->bWorkerAcquiredGIL);
 TestTrue(TEXT("Python encoding returns with GIL restored"), PyGILState_Check() != 0);
 Provider->bProbeGIL = false;
 // A worker may take the interpreter lock, but cannot use this game-thread context.
 Py_INCREF(Context);
 auto OffThread = Async(EAsyncExecution::Thread, [Context]()
 {
  const auto WorkerGIL = PyGILState_Ensure();
  PyObject* Result = PyObject_CallMethod(Context, "inspect_catalog", nullptr);
  const bool bRejected = Result == nullptr && PyErr_Occurred() != nullptr;
  PyErr_Clear(); Py_XDECREF(Result); Py_DECREF(Context);
  PyGILState_Release(WorkerGIL); return bRejected;
 });
 bool bOffThreadFinished = false;
 Py_BEGIN_ALLOW_THREADS
 bOffThreadFinished = OffThread.WaitFor(FTimespan::FromSeconds(2));
 Py_END_ALLOW_THREADS
 TestTrue(TEXT("worker context call terminates within bound"), bOffThreadFinished);
 TestTrue(TEXT("worker context access remains forbidden"), bOffThreadFinished && OffThread.Get());


	TestTypeHardening(*this, Module, Context, Proxy);
	TestCatalog(*this, Context);
	TestFailureClosedInputs(*this, Context, Proxy, ContextRuntime);
 TestTrue(TEXT("failure paths restore GIL before Python errors and decrefs"), PyGILState_Check() != 0);
	TestDiagnosticFidelity(*this, Context, *Provider);
	TestNestedMutation(*this, Context, *Provider, ContextRuntime);
	TestOutcomeTransportLimits(*this, Context, *Provider);
	TestReturnedHandleBounds(*this, Context, *Provider);
	TestStaleAndForgedHandles(*this, Context, Proxy, Handles, Provider);
	TestValueBounds(*this);
	TestExpiredBinding(*this, Binding, Context, Proxy);
	Py_DECREF(Proxy);
	Py_DECREF(Context);
	Py_DECREF(Module);
	AssetDocumentPythonRemoveNativeModule();
	PyGILState_Release(Gil);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentHandleHandoffOpaqueBoundaryTest,
	"UEAssetDoc.HandleHandoff.OpaqueBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHandleHandoffOpaqueBoundaryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	TestNotNull(TEXT("Engine Python plugin is loaded"), Python);
	if (Python == nullptr || !Python->IsPythonInitialized())
	{
		return false;
	}
	PyGILState_STATE Gil = PyGILState_Ensure();
	const FAssetDocumentContractValidationResult Install =
		AssetDocumentPythonInstallNativeModule();
	TestTrue(TEXT("typed native module installs"), Install.bValid);
	TSharedRef<FNativeModuleTestProvider> EditorProvider =
		MakeShared<FNativeModuleTestProvider>(MakeDescriptors());
	const FAssetDocumentPrimitiveRegistryBuildResult Build =
		FAssetDocumentPrimitiveRegistryRuntime::CreateFrozen(
			MakeHandoffNativeRegistryInput(EditorProvider));
	TestTrue(TEXT("handoff catalog freezes"), Build.Registry.IsValid());
	if (!Build.Registry.IsValid())
	{
		AssetDocumentPythonRemoveNativeModule();
		PyGILState_Release(Gil);
		return false;
	}
	FNativeModuleTestTransaction Transaction;
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = Transaction.GetMutationToken().RequestId;
	Metadata.PinnedRuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
	Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Handles =
		MakeShared<FAssetDocumentHandleRegistryRuntime>();
	FAssetDocumentNativeRequestContextRuntime ContextRuntime(
		Metadata,
		Transaction.GetTransactionId(),
		Build.Registry.ToSharedRef(),
		Handles,
		&Transaction);
	TSharedRef<FAssetDocumentPythonDispatchBindingState> Binding =
		AssetDocumentPythonOpenDispatchBinding(ContextRuntime);
	PyObject* Context = AssetDocumentPythonCreateRequestContextProxy(Binding);
	const FAssetDocumentOpaqueHandle SourceHandle = IssueNativeHandoffSource(
		*Handles, Metadata, Transaction.GetTransactionId());
	PyObject* Source = AssetDocumentPythonCreateOpaqueHandleProxy(
		Binding->BindingId, SourceHandle);

	TestTrue(TEXT("RequestContext has exact six public members"),
		HasExactContextMembers(GetPublicMembers(Context)));
	PyObject* Method = PyObject_GetAttrString(Context, "handoff");
	TestNotNull(TEXT("request context exposes handoff"), Method);
	if (Method == nullptr)
	{
		TakePythonError();
		Py_DECREF(Source);
		Py_DECREF(Context);
		AssetDocumentPythonCloseDispatchBinding(Binding);
		AssetDocumentPythonRemoveNativeModule();
		PyGILState_Release(Gil);
		return false;
	}
	Py_DECREF(Method);

	PyObject* Target = PyUnicode_FromString("animation");
	PyObject* Mutable = PyUnicode_FromString("mutable");
	PyObject* TargetHandle = CallHandoff(
		Context, Source, Target, Mutable);
	TestNotNull(TEXT("exact positional handoff succeeds"), TargetHandle);
	TestTrue(TEXT("handoff returns exact opaque proxy"),
		AssetDocumentPythonIsOpaqueHandleProxy(TargetHandle));
	if (TargetHandle != nullptr)
	{
		TestTrue(TEXT("returned handle exposes zero public metadata members"),
			GetPublicMembers(TargetHandle).IsEmpty());
		PyObject* Repr = PyObject_Repr(TargetHandle);
		TestEqual(TEXT("returned handle repr is redacted"),
			FString(UTF8_TO_TCHAR(PyUnicode_AsUTF8(Repr))),
			FString(TEXT("<_assetdocument_native.OpaqueHandle redacted>")));
		Py_DECREF(Repr);
		PyObject* Module = PyImport_ImportModule("_assetdocument_native");
		PyObject* HandleType = PyObject_GetAttrString(Module, "OpaqueHandle");
		TestProxyTypeHardening(
			*this, HandleType, TargetHandle,
			TEXT("returned OpaqueHandle rejects construction"));
		Py_DECREF(HandleType);
		Py_DECREF(Module);
		PyObject* Json = PyImport_ImportModule("json");
		PyObject* Dumps = PyObject_GetAttrString(Json, "dumps");
		PyObject* Serialized = PyObject_CallFunctionObjArgs(
			Dumps, TargetHandle, nullptr);
		TestNull(TEXT("returned handle rejects JSON serialization"), Serialized);
		Py_XDECREF(Serialized);
		Py_DECREF(Dumps);
		Py_DECREF(Json);
		TakePythonError();
	}

	TestTrue(TEXT("invalid access string fails closed"),
		CallHandoffAndTakeError(
			Context, Source, "animation", "write").Contains(
				TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	PyObject* Forged = PyDict_New();
	TestTrue(TEXT("forged handle fails closed"),
		CallHandoffAndTakeError(
			Context, Forged, "animation", "mutable").Contains(
				TEXT("AD.RUNTIME.STALE_HANDLE")));
	Py_DECREF(Forged);
	TSharedRef<FAssetDocumentPythonDispatchBindingState> OtherBinding =
		AssetDocumentPythonOpenDispatchBinding(ContextRuntime);
	PyObject* WrongBinding = AssetDocumentPythonCreateOpaqueHandleProxy(
		OtherBinding->BindingId, SourceHandle);
	TestTrue(TEXT("wrong binding fails closed"),
		CallHandoffAndTakeError(
			Context, WrongBinding, "animation", "mutable").Contains(
				TEXT("AD.RUNTIME.STALE_HANDLE")));
	Py_DECREF(WrongBinding);
	AssetDocumentPythonCloseDispatchBinding(OtherBinding);

	PyObject* TooFew = PyTuple_Pack(2, Source, Target);
	PyObject* TooFewResult = PyObject_Call(Method =
		PyObject_GetAttrString(Context, "handoff"), TooFew, nullptr);
	TestNull(TEXT("handoff rejects wrong arity"), TooFewResult);
	Py_XDECREF(TooFewResult);
	Py_DECREF(Method);
	Py_DECREF(TooFew);
	TakePythonError();
	PyObject* Arguments = PyTuple_Pack(3, Source, Target, Mutable);
	PyObject* Keywords = PyDict_New();
	PyDict_SetItemString(Keywords, "access", Mutable);
	Method = PyObject_GetAttrString(Context, "handoff");
	PyObject* KeywordResult = PyObject_Call(Method, Arguments, Keywords);
	TestNull(TEXT("handoff rejects keyword arguments"), KeywordResult);
	Py_XDECREF(KeywordResult);
	Py_DECREF(Method);
	Py_DECREF(Keywords);
	Py_DECREF(Arguments);
	TakePythonError();

	Py_XDECREF(TargetHandle);
	Py_DECREF(Mutable);
	Py_DECREF(Target);
	AssetDocumentPythonCloseDispatchBinding(Binding);
	TestTrue(TEXT("closed context handoff fails closed"),
		CallHandoffAndTakeError(
			Context, Source, "animation", "mutable").Contains(
				TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	Py_DECREF(Source);
	Py_DECREF(Context);
	// The same real native registry and opaque bridge admit Validate only for
	// an issued read-only Asset source without an active transaction.
	FAssetDocumentRequestContextMetadata ValidateMetadata = Metadata;
	ValidateMetadata.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	const auto ReadOnlySource = IssueNativeHandoffSource(*Handles, ValidateMetadata,
		Transaction.GetTransactionId(), EAssetDocumentHandleAccess::ReadOnly);
	FAssetDocumentNativeRequestContextRuntime ValidateRuntime(ValidateMetadata,
		Transaction.GetTransactionId(), Build.Registry.ToSharedRef(), Handles, nullptr);
	const auto ValidateBinding = AssetDocumentPythonOpenDispatchBinding(ValidateRuntime);
	PyObject* ValidateProxy = AssetDocumentPythonCreateRequestContextProxy(ValidateBinding);
	PyObject* ReadOnlyProxy = AssetDocumentPythonCreateOpaqueHandleProxy(ValidateBinding->BindingId, ReadOnlySource);
	PyObject* ReadOnlyAccess = PyUnicode_FromString("read_only");
	PyObject* ReadOnlyTarget = PyUnicode_FromString("animation");
	PyObject* ValidateAlias = CallHandoff(ValidateProxy, ReadOnlyProxy, ReadOnlyTarget, ReadOnlyAccess);
	TestNotNull(TEXT("Validate read-only handoff crosses the real opaque bridge"), ValidateAlias);
	if (ValidateAlias)
	{
		TestTrue(TEXT("Validate returns an exact opaque proxy"), AssetDocumentPythonIsOpaqueHandleProxy(ValidateAlias));
		TestTrue(TEXT("Validate alias exposes no metadata members"), GetPublicMembers(ValidateAlias).IsEmpty());
	}
	else TakePythonError();
	TestTrue(TEXT("Validate opaque source cannot upgrade"), CallHandoffAndTakeError(
		ValidateProxy, ReadOnlyProxy, "animation", "mutable").Contains(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	PyObject* MutableInValidate = AssetDocumentPythonCreateOpaqueHandleProxy(ValidateBinding->BindingId, SourceHandle);
	TestTrue(TEXT("Validate opaque mutable source cannot downgrade"), CallHandoffAndTakeError(
		ValidateProxy, MutableInValidate, "animation", "read_only").Contains(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	Py_DECREF(MutableInValidate);
	FAssetDocumentNativeRequestContextRuntime ValidateWithTransaction(ValidateMetadata,
		Transaction.GetTransactionId(), Build.Registry.ToSharedRef(), Handles, &Transaction);
	const auto TransactionBinding = AssetDocumentPythonOpenDispatchBinding(ValidateWithTransaction);
	PyObject* TransactionProxy = AssetDocumentPythonCreateRequestContextProxy(TransactionBinding);
	PyObject* TransactionSource = AssetDocumentPythonCreateOpaqueHandleProxy(TransactionBinding->BindingId, ReadOnlySource);
	TestTrue(TEXT("Validate opaque read-only source rejects an active transaction"), CallHandoffAndTakeError(
		TransactionProxy, TransactionSource, "animation", "read_only").Contains(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	AssetDocumentPythonCloseDispatchBinding(TransactionBinding);
	Py_DECREF(TransactionSource); Py_DECREF(TransactionProxy);
	AssetDocumentPythonCloseDispatchBinding(ValidateBinding);
	TestTrue(TEXT("closed Validate context retains its handoff rejection"), CallHandoffAndTakeError(
		ValidateProxy, ReadOnlyProxy, "animation", "read_only").Contains(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	Py_XDECREF(ValidateAlias); Py_DECREF(ReadOnlyTarget); Py_DECREF(ReadOnlyAccess);
	Py_DECREF(ReadOnlyProxy); Py_DECREF(ValidateProxy);
	AssetDocumentPythonRemoveNativeModule();
	PyGILState_Release(Gil);
	return true;
}
