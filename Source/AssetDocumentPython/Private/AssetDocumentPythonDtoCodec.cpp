// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPythonDtoCodec.h"

#include "AssetDocumentPythonNativeModule.h"

namespace
{
constexpr int32 MaximumDepth = 64;
// Per-call values (invocation arguments, outcomes, diagnostics); whole documents use
// RuntimeDocumentMaximumNodes (AssetDocumentRuntimeTypes.h).
constexpr int32 MaximumNodes = UE::AssetDocument::RuntimeTypes::Private::RuntimeValueMaximumNodes;
constexpr int32 MaximumDocumentNodes = UE::AssetDocument::RuntimeTypes::Private::RuntimeDocumentMaximumNodes;
constexpr Py_ssize_t MaximumStringBytes = 1024 * 1024;
// A bytes leaf carries a whole canonical document (a 20-field DataTable row is ~400 bytes). It is a
// memory sanity bound, set above what the 2^20-node document budget can produce for ordinary
// documents, so the node budget (not this cap) limits document size. It was 8 MiB, which stopped
// DataTables at about 18,000 rows.
constexpr Py_ssize_t MaximumBytesLeaf = 64 * 1024 * 1024;

struct FValueBudget final
{
	int32 RemainingNodes = MaximumNodes;
};

PyObject* EncodeDocumentValue(const FAssetDocumentRuntimeValueDto& Value);
bool DecodeDocumentValue(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic);

void SetDiagnostic(FAssetDocumentRuntimeDiagnostic& Diagnostic, const TCHAR* Code, const TCHAR* Message)
{
	Diagnostic.Path = TEXT("/"); Diagnostic.Code = Code; Diagnostic.Message = Message;
}

PyObject* FailPython(const TCHAR* Code, const TCHAR* Message)
{
	const FString Combined = FString(Code) + TEXT(": ") + Message;
	PyErr_SetString(PyExc_RuntimeError, TCHAR_TO_UTF8(*Combined));
	return nullptr;
}

bool FailDecode(FAssetDocumentRuntimeDiagnostic& Diagnostic, const TCHAR* Code, const TCHAR* Message)
{
	SetDiagnostic(Diagnostic, Code, Message);
	return false;
}

struct FEncodedField final
{
	const char* Key; PyObject* Value;
};

bool SetFields(PyObject* Dictionary, std::initializer_list<FEncodedField> Fields)
{
	bool bValid = Dictionary != nullptr;
	for (const FEncodedField& Field : Fields)
	{
		if (Field.Value == nullptr)
		{
			bValid = false;
			continue;
		}
		if (Dictionary != nullptr && PyDict_SetItemString(Dictionary, Field.Key, Field.Value) != 0)
		{
			bValid = false;
		}
		Py_DECREF(Field.Value);
	}
	return bValid;
}

PyObject* EncodeString(const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	if (Utf8.Length() > MaximumStringBytes)
	{
		return FailPython(TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"), TEXT("A string exceeds the Host transport limit."));
	}
	return PyUnicode_FromStringAndSize(Utf8.Get(), Utf8.Length());
}

bool DecodeString(PyObject* Object, FString& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	if (!PyUnicode_CheckExact(Object))
	{
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"),
			TEXT("Expected an exact string."));
	}
	Py_ssize_t Length = 0;
	const char* Utf8 = PyUnicode_AsUTF8AndSize(Object, &Length);
	if (Utf8 == nullptr || Length > MaximumStringBytes)
	{
		PyErr_Clear();
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"),
			TEXT("A string exceeds the Host transport limit."));
	}
	FUTF8ToTCHAR Converted(Utf8, static_cast<int32>(Length));
	OutValue = FString(Converted.Length(), Converted.Get());
	return true;
}

bool HasExactKeys(PyObject* Object, TConstArrayView<const char*> Keys)
{
	if (!PyDict_CheckExact(Object) || PyDict_Size(Object) != Keys.Num())
	{
		return false;
	}
	for (const char* Key : Keys)
	{
		if (PyDict_GetItemString(Object, Key) == nullptr)
		{
			return false;
		}
	}
	return true;
}

bool ConsumeBudget(FValueBudget& Budget, int32 Depth)
{
	if (Depth > MaximumDepth || Budget.RemainingNodes <= 0)
	{
		return false;
	}
	--Budget.RemainingNodes;
	return true;
}

PyObject* EncodeValueInternal(const FAssetDocumentRuntimeValueDto& Value, FValueBudget& Budget, int32 Depth);

PyObject* EncodeValueTuple(TConstArrayView<FAssetDocumentRuntimeValueDto> Values, FValueBudget& Budget, int32 Depth)
{
	PyObject* Tuple = PyTuple_New(Values.Num());
	for (int32 Index = 0; Tuple != nullptr && Index < Values.Num(); ++Index)
	{
		PyObject* Item = EncodeValueInternal(Values[Index], Budget, Depth + 1);
		if (Item == nullptr || PyTuple_SetItem(Tuple, Index, Item) != 0)
		{
			Py_XDECREF(Item);
			Py_CLEAR(Tuple);
		}
	}
	return Tuple;
}

PyObject* EncodeValueObject(const FAssetDocumentRuntimeValueDto& Value, FValueBudget& Budget, int32 Depth)
{
	PyObject* Dictionary = PyDict_New();
	for (int32 Index = 0; Dictionary != nullptr && Index < Value.Elements.Num(); ++Index)
	{
		PyObject* Item = EncodeValueInternal(Value.Elements[Index], Budget, Depth + 1);
		if (Item == nullptr || PyDict_SetItemString(
			Dictionary, TCHAR_TO_UTF8(*Value.ObjectKeys[Index]), Item) != 0)
		{
			Py_XDECREF(Item);
			Py_CLEAR(Dictionary);
			continue;
		}
		Py_DECREF(Item);
	}
	return Dictionary;
}

PyObject* EncodeScalarValue(const FAssetDocumentRuntimeValueDto& Value)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return Py_NewRef(Py_None);
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Boolean)
	{
		return PyBool_FromLong(Value.BooleanValue ? 1 : 0);
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		return PyLong_FromLongLong(Value.IntegerValue);
	}
	return PyFloat_FromDouble(Value.NumberValue);
}

PyObject* EncodeTextValue(const FAssetDocumentRuntimeValueDto& Value)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
	{
		return EncodeString(Value.StringValue);
	}
	if (Value.BytesValue.Num() > MaximumBytesLeaf)
	{
		return FailPython(TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"),
			TEXT("A bytes leaf exceeds the Host transport limit."));
	}
	return PyBytes_FromStringAndSize(reinterpret_cast<const char*>(Value.BytesValue.GetData()), Value.BytesValue.Num());
}

PyObject* EncodeContainerValue(const FAssetDocumentRuntimeValueDto& Value, FValueBudget& Budget, int32 Depth)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		return EncodeValueTuple(Value.Elements, Budget, Depth);
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		return EncodeValueObject(Value, Budget, Depth);
	}
	return FailPython(TEXT("AD.RUNTIME.INVALID_REQUEST_DTO"),
		TEXT("RuntimeValue kind is outside the frozen enum."));
}

PyObject* EncodeValueInternal(const FAssetDocumentRuntimeValueDto& Value, FValueBudget& Budget, int32 Depth)
{
	if (!ConsumeBudget(Budget, Depth))
	{
		return FailPython(TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"),
			TEXT("RuntimeValue exceeds the depth or total-node limit."));
	}
	if (Value.Kind <= EAssetDocumentRuntimeValueKind::Number)
	{
		return EncodeScalarValue(Value);
	}
	if (Value.Kind <= EAssetDocumentRuntimeValueKind::Bytes)
	{
		return EncodeTextValue(Value);
	}
	return EncodeContainerValue(Value, Budget, Depth);
}

bool DecodeValueInternal(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic, FValueBudget& Budget, int32 Depth);

bool DecodeSequence(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic, FValueBudget& Budget, int32 Depth)
{
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Array;
	const Py_ssize_t Count = PyTuple_Size(Object);
	OutValue.Elements.Reserve(Count);
	for (Py_ssize_t Index = 0; Index < Count; ++Index)
	{
		FAssetDocumentRuntimeValueDto& Item = OutValue.Elements.AddDefaulted_GetRef();
		if (!DecodeValueInternal(PyTuple_GetItem(Object, Index), Item, OutDiagnostic, Budget, Depth + 1))
		{
			return false;
		}
	}
	return true;
}

bool DecodeObject(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic, FValueBudget& Budget, int32 Depth)
{
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Object;
	PyObject* Key = nullptr; PyObject* Value = nullptr; Py_ssize_t Position = 0;
	while (PyDict_Next(Object, &Position, &Key, &Value))
	{
		FString& Name = OutValue.ObjectKeys.AddDefaulted_GetRef();
		if (!DecodeString(Key, Name, OutDiagnostic))
		{
			return false;
		}
		FAssetDocumentRuntimeValueDto& Item = OutValue.Elements.AddDefaulted_GetRef();
		if (!DecodeValueInternal(Value, Item, OutDiagnostic, Budget, Depth + 1))
		{
			return false;
		}
	}
	return true;
}

bool DecodeIntegerScalar(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue)
{
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Integer;
	OutValue.IntegerValue = PyLong_AsLongLong(Object);
	if (PyErr_Occurred())
	{
		PyErr_Clear();
		return false;
	}
	return true;
}

bool DecodeNumberScalar(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue,
	FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Number;
	OutValue.NumberValue = PyFloat_AsDouble(Object);
	return FMath::IsFinite(OutValue.NumberValue)
		|| FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("RuntimeValue number must be finite."));
}

bool DecodeScalar(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	if (Object == Py_None)
	{
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Null;
		return true;
	}
	if (PyBool_Check(Object))
	{
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Boolean;
		OutValue.BooleanValue = Object == Py_True;
		return true;
	}
	if (PyLong_CheckExact(Object))
	{
		return DecodeIntegerScalar(Object, OutValue);
	}
	if (PyFloat_CheckExact(Object))
	{
		return DecodeNumberScalar(Object, OutValue, OutDiagnostic);
	}
	return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"),
		TEXT("RuntimeValue contains an unsupported scalar."));
}

bool IsScalarProjection(PyObject* Object)
{
	return Object == Py_None || PyBool_Check(Object) || PyLong_CheckExact(Object) || PyFloat_CheckExact(Object);
}

bool IsTextProjection(PyObject* Object)
{
	return PyUnicode_CheckExact(Object) || PyBytes_CheckExact(Object);
}

bool DecodeContainer(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic, FValueBudget& Budget, int32 Depth)
{
	if (PyTuple_CheckExact(Object))
	{
		return DecodeSequence(Object, OutValue, OutDiagnostic, Budget, Depth);
	}
	if (PyDict_CheckExact(Object))
	{
		return DecodeObject(Object, OutValue, OutDiagnostic, Budget, Depth);
	}
	return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"),
		TEXT("RuntimeValue must use exact built-in projection types."));
}

bool DecodeTextOrBytes(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	if (PyUnicode_CheckExact(Object))
	{
		OutValue.Kind = EAssetDocumentRuntimeValueKind::String;
		return DecodeString(Object, OutValue.StringValue, OutDiagnostic);
	}
	if (!PyBytes_CheckExact(Object))
	{
		return false;
	}
	const Py_ssize_t Count = PyBytes_Size(Object);
	if (Count > MaximumBytesLeaf)
	{
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"),
			TEXT("A bytes leaf exceeds the Host transport limit."));
	}
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Bytes;
	OutValue.BytesValue.Append(reinterpret_cast<const uint8*>(PyBytes_AsString(Object)), Count);
	return true;
}

bool DecodeValueInternal(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic, FValueBudget& Budget, int32 Depth)
{
	if (!ConsumeBudget(Budget, Depth))
	{
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"),
			TEXT("RuntimeValue exceeds the depth or total-node limit."));
	}
	if (IsScalarProjection(Object))
	{
		return DecodeScalar(Object, OutValue, OutDiagnostic);
	}
	if (IsTextProjection(Object))
	{
		return DecodeTextOrBytes(Object, OutValue, OutDiagnostic);
	}
	return DecodeContainer(Object, OutValue, OutDiagnostic, Budget, Depth);
}

PyObject* EncodeStringTuple(TConstArrayView<FString> Values)
{
	PyObject* Tuple = PyTuple_New(Values.Num());
	for (int32 Index = 0; Tuple != nullptr && Index < Values.Num(); ++Index)
	{
		PyObject* Item = EncodeString(Values[Index]);
		if (Item == nullptr || PyTuple_SetItem(Tuple, Index, Item) != 0)
		{
			Py_XDECREF(Item);
			Py_CLEAR(Tuple);
		}
	}
	return Tuple;
}

template <typename EnumType>
PyObject* EncodeEnumTuple(TConstArrayView<EnumType> Values)
{
	PyObject* Tuple = PyTuple_New(Values.Num());
	for (int32 Index = 0; Tuple != nullptr && Index < Values.Num(); ++Index)
	{
		PyObject* Item = PyLong_FromLong(static_cast<uint8>(Values[Index]));
		if (Item == nullptr || PyTuple_SetItem(Tuple, Index, Item) != 0)
		{
			Py_XDECREF(Item);
			Py_CLEAR(Tuple);
		}
	}
	return Tuple;
}

PyObject* EncodeDiagnostic(const FAssetDocumentRuntimeDiagnostic& Diagnostic);

PyObject* EncodeDiagnosticContext(const FAssetDocumentRuntimeDiagnosticContext& Context)
{
	PyObject* Result = PyDict_New();
	FValueBudget Budget;
	if (!SetFields(Result, {{"key", EncodeString(Context.Key)},
		{"value", EncodeValueInternal(Context.Value, Budget, 0)}}))
	{
		Py_CLEAR(Result);
	}
	return Result;
}

PyObject* EncodeDiagnosticContexts(TConstArrayView<FAssetDocumentRuntimeDiagnosticContext> Contexts)
{
	PyObject* Tuple = PyTuple_New(Contexts.Num());
	for (int32 Index = 0; Tuple != nullptr && Index < Contexts.Num(); ++Index)
	{
		PyObject* Item = EncodeDiagnosticContext(Contexts[Index]);
		if (Item == nullptr || PyTuple_SetItem(Tuple, Index, Item) != 0)
		{
			Py_XDECREF(Item);
			Py_CLEAR(Tuple);
		}
	}
	return Tuple;
}

PyObject* EncodeDiagnostic(const FAssetDocumentRuntimeDiagnostic& Diagnostic)
{
	PyObject* Result = PyDict_New();
	if (!SetFields(Result, {
		{"path", EncodeString(Diagnostic.Path)}, {"code", EncodeString(Diagnostic.Code)},
		{"message", EncodeString(Diagnostic.Message)},
		{"severity", PyLong_FromLong(static_cast<uint8>(Diagnostic.Severity))},
		{"context", EncodeDiagnosticContexts(Diagnostic.Context)}}))
	{
		Py_CLEAR(Result);
	}
	return Result;
}

template <typename ItemType, PyObject* (*Encoder)(const ItemType&)>
PyObject* EncodeTuple(TConstArrayView<ItemType> Items)
{
	PyObject* Tuple = PyTuple_New(Items.Num());
	for (int32 Index = 0; Tuple != nullptr && Index < Items.Num(); ++Index)
	{
		PyObject* Item = Encoder(Items[Index]);
		if (Item == nullptr || PyTuple_SetItem(Tuple, Index, Item) != 0)
		{
			Py_XDECREF(Item);
			Py_CLEAR(Tuple);
		}
	}
	return Tuple;
}

PyObject* EncodeAdmission(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	PyObject* Result = PyDict_New();
	const bool bSet = SetFields(Result, {
		{"contract_version", PyLong_FromUnsignedLong(Evidence.ContractVersion)}, {"disposition", PyLong_FromLong(static_cast<uint8>(Evidence.Disposition))}, {"authority_path", PyLong_FromLong(static_cast<uint8>(Evidence.AuthorityPath))}, {"exact_engine_version", EncodeString(Evidence.ExactEngineVersion)},
		{"physical_owner", PyLong_FromLong(static_cast<uint8>(Evidence.PhysicalOwner))}, {"public_module", EncodeString(Evidence.PublicModule)}, {"public_header", EncodeString(Evidence.PublicHeader)}, {"exported_authoritative_api", EncodeString(Evidence.ExportedAuthoritativeApi)},
		{"authority_context_id", EncodeString(Evidence.AuthorityContextId)}, {"primary_operation_id", EncodeString(Evidence.PrimaryOperationId)},
		{"identity_lifetime_contract_id", EncodeString(Evidence.IdentityLifetimeContractId)}, {"transaction_contract_id", EncodeString(Evidence.TransactionContractId)}, {"rollback_contract_id", EncodeString(Evidence.RollbackContractId)},
		{"completion_contract_id", EncodeString(Evidence.CompletionContractId)}, {"save_reload_evidence_id", EncodeString(Evidence.SaveReloadEvidenceId)}, {"failure_injection_test_ids", EncodeStringTuple(Evidence.FailureInjectionTestIds)}, {"verification_test_ids", EncodeStringTuple(Evidence.VerificationTestIds)},
		{"source_marker", EncodeString(Evidence.SourceMarker)}, {"raw_lifecycle_allowlist_id", EncodeString(Evidence.RawLifecycleAllowlistId)}, {"governance_id", EncodeString(Evidence.GovernanceId)}, {"independent_candidate_review_id", EncodeString(Evidence.IndependentCandidateReviewId)}, {"independent_bridge_review_id", EncodeString(Evidence.IndependentBridgeReviewId)},
		{"helper_replacement_condition", EncodeString(Evidence.HelperReplacementCondition)}, {"removal_condition", EncodeString(Evidence.RemovalCondition)}, {"authority_fingerprint", EncodeString(Evidence.AuthorityFingerprint)}});
	if (!bSet) { Py_CLEAR(Result); }
	return Result;
}

PyObject* EncodeArgumentSchema(const FAssetDocumentPrimitiveHandleArgumentSchema& Schema)
{
	PyObject* Result = PyDict_New();
	if (!SetFields(Result, {{"name", EncodeString(Schema.Name)}, {"kind", PyLong_FromLong(static_cast<uint8>(Schema.Kind))}, {"required_access", PyLong_FromLong(static_cast<uint8>(Schema.RequiredAccess))}, {"required", PyBool_FromLong(Schema.bRequired)}})) { Py_CLEAR(Result); }
	return Result;
}

PyObject* EncodeResultSchema(const FAssetDocumentPrimitiveHandleResultSchema& Schema)
{
	PyObject* Result = PyDict_New();
	if (!SetFields(Result, {{"name", EncodeString(Schema.Name)}, {"kind", PyLong_FromLong(static_cast<uint8>(Schema.Kind))}, {"granted_access", PyLong_FromLong(static_cast<uint8>(Schema.GrantedAccess))}, {"cardinality", PyLong_FromLong(static_cast<uint8>(Schema.Cardinality))}})) { Py_CLEAR(Result); }
	return Result;
}

PyObject* EncodeDescriptor(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	PyObject* Result = PyDict_New();
	const bool bSet = SetFields(Result, {
		{"primitive_id", EncodeString(Descriptor.PrimitiveId)}, {"version", PyLong_FromUnsignedLong(Descriptor.Version)},
		{"provider_id", EncodeString(Descriptor.ProviderId)}, {"owner", PyLong_FromLong(static_cast<uint8>(Descriptor.Owner))}, {"execution_thread", PyLong_FromLong(static_cast<uint8>(Descriptor.ExecutionThread))}, {"access", PyLong_FromLong(static_cast<uint8>(Descriptor.Access))},
		{"allowed_phases", EncodeEnumTuple<EAssetDocumentRuntimeRequestPhase>(Descriptor.AllowedPhases)}, {"allowed_handle_kinds", EncodeEnumTuple<EAssetDocumentHandleKind>(Descriptor.AllowedHandleKinds)},
		{"handle_argument_schemas", EncodeTuple<FAssetDocumentPrimitiveHandleArgumentSchema, EncodeArgumentSchema>(Descriptor.HandleArgumentSchemas)}, {"handle_result_schemas", EncodeTuple<FAssetDocumentPrimitiveHandleResultSchema, EncodeResultSchema>(Descriptor.HandleResultSchemas)},
		{"requires_active_transaction", PyBool_FromLong(Descriptor.bRequiresActiveTransaction)}, {"input_schema_id", EncodeString(Descriptor.InputSchemaId)}, {"output_schema_id", EncodeString(Descriptor.OutputSchemaId)}, {"mutation_footprint_schema_id", EncodeString(Descriptor.MutationFootprintSchemaId)},
		{"stable_diagnostic_codes", EncodeStringTuple(Descriptor.StableDiagnosticCodes)}, {"required_consumer_ids", EncodeStringTuple(Descriptor.RequiredConsumerIds)}, {"admission", EncodeAdmission(Descriptor.Admission)}});
	if (!bSet) { Py_CLEAR(Result); }
	return Result;
}

PyObject* EncodeProvider(const FAssetDocumentProviderAvailability& Provider)
{
	PyObject* Result = PyDict_New();
	PyObject* Diagnostic = Provider.Diagnostic.IsSet() ? EncodeDiagnostic(Provider.Diagnostic.GetValue()) : Py_NewRef(Py_None);
	if (!SetFields(Result, {{"provider_id", EncodeString(Provider.ProviderId)}, {"state", PyLong_FromLong(static_cast<uint8>(Provider.State))}, {"diagnostic", Diagnostic}})) { Py_CLEAR(Result); }
	return Result;
}

const FAssetDocumentPrimitiveHandleResultSchema* FindCodecResultSchema(TConstArrayView<FAssetDocumentPrimitiveHandleResultSchema> Schemas, FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Schemas)
	{
		if (Schema.Name == Name)
		{
			return &Schema;
		}
	}
	return nullptr;
}

bool IsCodecCardinalityValid(EAssetDocumentHandleResultCardinality Cardinality, int32 Count)
{
	switch (Cardinality)
	{
	case EAssetDocumentHandleResultCardinality::ExactlyOne: return Count == 1;
	case EAssetDocumentHandleResultCardinality::ZeroOrOne: return Count <= 1;
	case EAssetDocumentHandleResultCardinality::ZeroOrMore: return true;
	case EAssetDocumentHandleResultCardinality::OneOrMore: return Count >= 1;
	default: return false;
	}
}

bool IsHandleProjectionSchemaValid(const FAssetDocumentPrimitiveHandleResultSchema* Schema, int32 Count)
{
	return Schema != nullptr && IsCodecCardinalityValid(Schema->Cardinality, Count);
}

PyObject* EncodeHandleProxyTuple(TConstArrayView<FAssetDocumentOpaqueHandle> Handles, uint64 ContextBindingId)
{
	PyObject* Result = PyTuple_New(Handles.Num());
	for (int32 Index = 0; Result != nullptr && Index < Handles.Num(); ++Index)
	{
		PyObject* Proxy = AssetDocumentPythonCreateOpaqueHandleProxy(ContextBindingId, Handles[Index]);
		if (Proxy == nullptr)
		{
			Py_CLEAR(Result);
			continue;
		}
		if (PyTuple_SetItem(Result, Index, Proxy) != 0)
		{
			Py_DECREF(Proxy);
			Py_CLEAR(Result);
		}
	}
	return Result;
}

PyObject* EncodeHandleResult(const FAssetDocumentPrimitiveHandleResult& Group, uint64 ContextBindingId, const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FAssetDocumentPrimitiveHandleResultSchema* Schema = FindCodecResultSchema(Descriptor.HandleResultSchemas, Group.Name);
	if (!IsHandleProjectionSchemaValid(Schema, Group.Handles.Num()))
	{
		return FailPython(TEXT("AD.RUNTIME.PRIMITIVE_OUTCOME_REJECTED"),
			TEXT("Handle-result projection violates the frozen schema."));
	}
	PyObject* Handles = EncodeHandleProxyTuple(Group.Handles, ContextBindingId);
	PyObject* Result = PyDict_New();
	if (!SetFields(Result, {{"name", EncodeString(Group.Name)}, {"handles", Handles}}))
	{
		Py_CLEAR(Result);
	}
	return Result;
}

PyObject* EncodeHandleResults(TConstArrayView<FAssetDocumentPrimitiveHandleResult> Groups, uint64 ContextBindingId, const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	PyObject* Tuple = PyTuple_New(Groups.Num());
	for (int32 Index = 0; Tuple != nullptr && Index < Groups.Num(); ++Index)
	{
		PyObject* Item = EncodeHandleResult(Groups[Index], ContextBindingId, Descriptor);
		if (Item == nullptr || PyTuple_SetItem(Tuple, Index, Item) != 0)
		{
			Py_XDECREF(Item);
			Py_CLEAR(Tuple);
		}
	}
	return Tuple;
}

PyObject* EncodeChanged(const FAssetDocumentChangedRecord& Changed)
{
	PyObject* Result = PyDict_New();
	if (!SetFields(Result, {{"authority_id", EncodeString(Changed.AuthorityId)},
		{"stable_object_id", EncodeString(Changed.StableObjectId)},
		{"path", EncodeString(Changed.Path)}, {"change_kind", EncodeString(Changed.ChangeKind)}}))
	{
		Py_CLEAR(Result);
	}
	return Result;
}

bool DecodeUnsigned(PyObject* Object, uint64& OutValue)
{
	if (!PyLong_CheckExact(Object))
	{
		return false;
	}
	OutValue = PyLong_AsUnsignedLongLong(Object);
	if (PyErr_Occurred())
	{
		PyErr_Clear();
		return false;
	}
	return true;
}

bool DecodeGuid(PyObject* Object, FGuid& OutGuid, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	FString Text;
	return DecodeString(Object, Text, OutDiagnostic)
		&& FGuid::ParseExact(Text, EGuidFormats::DigitsWithHyphens, OutGuid);
}

bool DecodeInvokeHandle(PyObject* Object, uint64 ContextBindingId, FAssetDocumentPrimitiveHandleArgument& OutArgument, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	static constexpr const char* Keys[] = {"name", "handle"};
	if (!HasExactKeys(Object, Keys)
		|| !DecodeString(PyDict_GetItemString(Object, "name"), OutArgument.Name, OutDiagnostic))
	{
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("Handle arguments require exact name and handle keys."));
	}
	return AssetDocumentPythonResolveOpaqueHandleProxy(PyDict_GetItemString(Object, "handle"), ContextBindingId, OutArgument.Handle, OutDiagnostic);
}

bool DecodeInvokeValueObject(PyObject* ValueArguments, FAssetDocumentPrimitiveInvocation& OutInvocation, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	if (!AssetDocumentPythonDecodeValue(ValueArguments, OutInvocation.ValueArguments, OutDiagnostic))
	{
		return false;
	}
	return OutInvocation.ValueArguments.Kind == EAssetDocumentRuntimeValueKind::Object;
}

bool IsBoundedHandleArgumentTuple(PyObject* HandleArguments)
{
	return PyTuple_CheckExact(HandleArguments) && PyTuple_Size(HandleArguments) <= 256;
}

bool IsCatalogSnapshotValid(const FAssetDocumentPrimitiveRegistrySnapshot& Snapshot)
{
	return Snapshot.bFrozen && Snapshot.RuntimeRevision > 0
		&& Snapshot.Descriptors.Num() <= MaximumNodes
		&& Snapshot.Providers.Num() <= MaximumNodes;
}

bool DecodeResultIdentity(PyObject* Object, uint64& Abi, FGuid& RequestId, uint64& Status, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	return DecodeUnsigned(PyDict_GetItemString(Object, "abi_version"), Abi)
		&& DecodeGuid(PyDict_GetItemString(Object, "request_id"), RequestId, OutDiagnostic)
		&& DecodeUnsigned(PyDict_GetItemString(Object, "terminal_status"), Status);
}

bool DecodeResultRevision(PyObject* Object, uint64& Phase, uint64& Revision, uint64& Codec)
{
	return DecodeUnsigned(PyDict_GetItemString(Object, "final_phase"), Phase)
		&& DecodeUnsigned(PyDict_GetItemString(Object, "runtime_revision"), Revision)
		&& DecodeUnsigned(PyDict_GetItemString(Object, "codec_version"), Codec);
}

bool DecodeOptionalCanonicalValue(PyObject* Object, FAssetDocumentRuntimeResultDto& OutResult, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	PyObject* Canonical = PyDict_GetItemString(Object, "canonical_value");
	if (Canonical == Py_None) { return true; }
	FAssetDocumentRuntimeValueDto Value;
	if (!DecodeDocumentValue(Canonical, Value, OutDiagnostic)) { return false; }
	OutResult.CanonicalValue = MoveTemp(Value); return true;
}

bool DecodeResultBytes(PyObject* Object, FAssetDocumentRuntimeResultDto& OutResult, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	PyObject* Bytes = PyDict_GetItemString(Object, "canonical_bytes");
	if (!PyBytes_CheckExact(Bytes)) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("Canonical bytes require an exact bytes value.")); }
	const Py_ssize_t Count = PyBytes_Size(Bytes);
	if (Count > MaximumBytesLeaf) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"), TEXT("Canonical bytes exceed the Host transport limit.")); }
	OutResult.CanonicalBytes.Append(reinterpret_cast<const uint8*>(PyBytes_AsString(Bytes)), Count); return true;
}

bool DecodeDiagnosticContext(PyObject* Object, FAssetDocumentRuntimeDiagnosticContext& OutContext, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	static constexpr const char* Keys[] = {"key", "value"};
	if (!HasExactKeys(Object, Keys)) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("Diagnostic context has the wrong key set.")); }
	return DecodeString(PyDict_GetItemString(Object, "key"), OutContext.Key, OutDiagnostic) & AssetDocumentPythonDecodeValue(PyDict_GetItemString(Object, "value"), OutContext.Value, OutDiagnostic);
}

bool DecodeDiagnostic(PyObject* Object, FAssetDocumentRuntimeDiagnostic& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	static constexpr const char* Keys[] = {"path", "code", "message", "severity", "context"};
	if (!HasExactKeys(Object, Keys)) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("Diagnostic has the wrong key set.")); }
	uint64 Severity = 0; bool bDecoded = DecodeString(PyDict_GetItemString(Object, "path"), OutValue.Path, OutDiagnostic) & DecodeString(PyDict_GetItemString(Object, "code"), OutValue.Code, OutDiagnostic) & DecodeString(PyDict_GetItemString(Object, "message"), OutValue.Message, OutDiagnostic) & DecodeUnsigned(PyDict_GetItemString(Object, "severity"), Severity);
	OutValue.Severity = static_cast<EAssetDocumentDiagnosticSeverity>(Severity); PyObject* Context = PyDict_GetItemString(Object, "context");
	if (!PyTuple_CheckExact(Context)) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("Diagnostic context requires an exact tuple.")); }
	for (Py_ssize_t Index = 0; Index < PyTuple_Size(Context); ++Index) { bDecoded &= DecodeDiagnosticContext(PyTuple_GetItem(Context, Index), OutValue.Context.AddDefaulted_GetRef(), OutDiagnostic); }
	return bDecoded;
}

bool DecodeResultDiagnostics(PyObject* Object, FAssetDocumentRuntimeResultDto& OutResult, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	PyObject* Diagnostics = PyDict_GetItemString(Object, "diagnostics"); if (!PyTuple_CheckExact(Diagnostics)) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("Diagnostics require an exact tuple.")); }
	if (PyTuple_Size(Diagnostics) > 256) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"), TEXT("Diagnostics violate the Host transport limit.")); }
	bool bDecoded = true;
	for (Py_ssize_t Index = 0; Index < PyTuple_Size(Diagnostics); ++Index) { bDecoded &= DecodeDiagnostic(PyTuple_GetItem(Diagnostics, Index), OutResult.Diagnostics.AddDefaulted_GetRef(), OutDiagnostic); }
	return bDecoded;
}

bool DecodeChanged(PyObject* Object, FAssetDocumentChangedRecord& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	static constexpr const char* Keys[] = {"authority_id", "stable_object_id", "path", "change_kind"};
	if (!HasExactKeys(Object, Keys)) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("Changed record has the wrong key set.")); }
	return DecodeString(PyDict_GetItemString(Object, "authority_id"), OutValue.AuthorityId, OutDiagnostic) & DecodeString(PyDict_GetItemString(Object, "stable_object_id"), OutValue.StableObjectId, OutDiagnostic) & DecodeString(PyDict_GetItemString(Object, "path"), OutValue.Path, OutDiagnostic) & DecodeString(PyDict_GetItemString(Object, "change_kind"), OutValue.ChangeKind, OutDiagnostic);
}

bool DecodeResultChangedSet(PyObject* Object, FAssetDocumentRuntimeResultDto& OutResult, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	PyObject* ChangedSet = PyDict_GetItemString(Object, "changed_set"); if (!PyTuple_CheckExact(ChangedSet)) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("Changed set requires an exact tuple.")); }
	if (PyTuple_Size(ChangedSet) > MaximumNodes) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"), TEXT("Changed set violates the Host transport limit.")); }
	bool bDecoded = true;
	for (Py_ssize_t Index = 0; Index < PyTuple_Size(ChangedSet); ++Index) { bDecoded &= DecodeChanged(PyTuple_GetItem(ChangedSet, Index), OutResult.ChangedSet.AddDefaulted_GetRef(), OutDiagnostic); }
	return bDecoded;
}
} // namespace

PyObject* AssetDocumentPythonEncodeValue(const FAssetDocumentRuntimeValueDto& Value, int32 Depth)
{
	const FAssetDocumentContractValidationResult Validation = ValidateAssetDocumentRuntimeValue(Value);
	if (!Validation.bValid)
	{
		return FailPython(TEXT("AD.RUNTIME.INVALID_REQUEST_DTO"),
			TEXT("RuntimeValue violates the frozen DTO contract."));
	}
	FValueBudget Budget;
	return EncodeValueInternal(Value, Budget, Depth);
}

bool AssetDocumentPythonDecodeValue(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic, int32 Depth)
{
	FValueBudget Budget;
	FAssetDocumentRuntimeValueDto Candidate;
	if (!DecodeValueInternal(Object, Candidate, OutDiagnostic, Budget, Depth))
	{
		return false;
	}
	const FAssetDocumentContractValidationResult Validation = ValidateAssetDocumentRuntimeValue(Candidate);
	if (!Validation.bValid)
	{
		OutDiagnostic = Validation.Diagnostics[0];
		return false;
	}
	OutValue = MoveTemp(Candidate);
	return true;
}

namespace
{
PyObject* EncodeDocumentValue(const FAssetDocumentRuntimeValueDto& Value)
{
	if (!ValidateAssetDocumentRuntimeDocument(Value).bValid)
	{
		return FailPython(TEXT("AD.RUNTIME.INVALID_REQUEST_DTO"),
			TEXT("Canonical document violates the frozen DTO contract."));
	}
	FValueBudget Budget{MaximumDocumentNodes};
	return EncodeValueInternal(Value, Budget, 0);
}

bool DecodeDocumentValue(PyObject* Object, FAssetDocumentRuntimeValueDto& OutValue, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	FValueBudget Budget{MaximumDocumentNodes};
	FAssetDocumentRuntimeValueDto Candidate;
	if (!DecodeValueInternal(Object, Candidate, OutDiagnostic, Budget, 0))
	{
		return false;
	}
	const FAssetDocumentContractValidationResult Validation = ValidateAssetDocumentRuntimeDocument(Candidate);
	if (!Validation.bValid)
	{
		OutDiagnostic = Validation.Diagnostics[0];
		return false;
	}
	OutValue = MoveTemp(Candidate);
	return true;
}
} // namespace

bool AssetDocumentPythonDecodeInvokeArguments(PyObject* ValueArguments, PyObject* HandleArguments, uint64 ContextBindingId, FAssetDocumentPrimitiveInvocation& OutInvocation, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	if (!DecodeInvokeValueObject(ValueArguments, OutInvocation, OutDiagnostic))
	{
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("Primitive value arguments must be an exact object projection."));
	}
	if (!IsBoundedHandleArgumentTuple(HandleArguments))
	{
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED"),
			TEXT("Handle arguments must be a bounded exact tuple."));
	}
	for (Py_ssize_t Index = 0; Index < PyTuple_Size(HandleArguments); ++Index)
	{
		FAssetDocumentPrimitiveHandleArgument& Argument = OutInvocation.HandleArguments.AddDefaulted_GetRef();
		if (!DecodeInvokeHandle(PyTuple_GetItem(HandleArguments, Index),
			ContextBindingId, Argument, OutDiagnostic))
		{
			return false;
		}
	}
	return true;
}

PyObject* AssetDocumentPythonEncodePrimitiveCatalog(const FAssetDocumentPrimitiveRegistrySnapshot& Snapshot, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	if (!IsCatalogSnapshotValid(Snapshot))
	{
		SetDiagnostic(OutDiagnostic, TEXT("AD.RUNTIME.REVISION_REGISTRY_INVALID"),
			TEXT("The primitive catalog is not a bounded frozen snapshot."));
		return FailPython(OutDiagnostic.Code.GetCharArray().GetData(),
			OutDiagnostic.Message.GetCharArray().GetData());
	}
	PyObject* Result = PyDict_New();
	if (!SetFields(Result, {{"runtime_revision", PyLong_FromUnsignedLongLong(Snapshot.RuntimeRevision)},
		{"frozen", PyBool_FromLong(Snapshot.bFrozen)},
		{"descriptors", EncodeTuple<FAssetDocumentPrimitiveDescriptor, EncodeDescriptor>(Snapshot.Descriptors)},
		{"providers", EncodeTuple<FAssetDocumentProviderAvailability, EncodeProvider>(Snapshot.Providers)}}))
	{
		Py_CLEAR(Result);
	}
	return Result;
}

PyObject* AssetDocumentPythonEncodePrimitiveOutcome(const FAssetDocumentPrimitiveOutcome& Outcome, const FAssetDocumentPrimitiveDescriptor& Descriptor, uint64 ContextBindingId, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	if (Outcome.HandleResults.Num() > MaximumNodes || Outcome.Diagnostics.Num() > 256 || Outcome.ChangedSet.Num() > MaximumNodes)
	{
		SetDiagnostic(OutDiagnostic, TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"),
			TEXT("Primitive outcome exceeds the Host transport limit."));
		return FailPython(TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"), OutDiagnostic.Message.GetCharArray().GetData());
	}
	PyObject* Result = PyDict_New();
	FValueBudget Budget;
	const bool bSet = SetFields(Result, {
		{"status", PyLong_FromLong(static_cast<uint8>(Outcome.Status))},
		{"value", EncodeValueInternal(Outcome.Value, Budget, 0)},
		{"handle_results", EncodeHandleResults(Outcome.HandleResults, ContextBindingId, Descriptor)},
		{"changed_set", EncodeTuple<FAssetDocumentChangedRecord, EncodeChanged>(Outcome.ChangedSet)},
		{"diagnostics", EncodeTuple<FAssetDocumentRuntimeDiagnostic, EncodeDiagnostic>(Outcome.Diagnostics)}});
	if (!bSet)
	{
		Py_CLEAR(Result);
	}
	return Result;
}

PyObject* AssetDocumentPythonEncodeRequest(const FAssetDocumentRuntimeRequestDto& Request)
{
	PyObject* Result = PyDict_New();
	PyObject* Cancellation = PyDict_New();
	SetFields(Cancellation, {{"sequence", PyLong_FromUnsignedLongLong(Request.Cancellation.Sequence)},
		{"requested", PyBool_FromLong(Request.Cancellation.bRequested)}});
	const bool bSet = SetFields(Result, {
		{"abi_version", PyLong_FromUnsignedLong(Request.AbiVersion)},
		{"request_id", EncodeString(Request.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower))},
		{"operation", PyLong_FromLong(static_cast<uint8>(Request.Operation))}, {"target_asset_path", EncodeString(Request.TargetAssetPath)},
		{"requested_profile_class_path", EncodeString(Request.RequestedProfileClassPath)}, {"requested_asset_class_path", EncodeString(Request.RequestedAssetClassPath)},
		{"canonical_document", EncodeDocumentValue(Request.CanonicalDocument)}, {"deadline_unix_milliseconds", PyLong_FromLongLong(Request.DeadlineUnixMilliseconds)},
		{"cancellation", Cancellation}, {"expected_runtime_revision", PyLong_FromUnsignedLongLong(Request.ExpectedRuntimeRevision)}});
	if (!bSet)
	{
		Py_CLEAR(Result);
	}
	return Result;
}

bool AssetDocumentPythonDecodeResult(PyObject* Object, FAssetDocumentRuntimeResultDto& OutResult, FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
{
	static constexpr const char* Keys[] = {
		"abi_version", "request_id", "terminal_status", "final_phase",
		"runtime_revision", "diagnostics", "canonical_value", "canonical_bytes",
		"semantic_hash", "codec_version", "changed_set"};
	if (!HasExactKeys(Object, Keys)) { return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"), TEXT("Result DTO has the wrong key set.")); }
	uint64 Abi = 0; uint64 Status = 0; uint64 Phase = 0; uint64 Revision = 0; uint64 Codec = 0;
	if (!DecodeResultIdentity(Object, Abi, OutResult.RequestId, Status, OutDiagnostic))
	{
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"),
			TEXT("Result DTO identity metadata is invalid."));
	}
	if (!DecodeResultRevision(Object, Phase, Revision, Codec))
	{
		return FailDecode(OutDiagnostic, TEXT("AD.RUNTIME.INVALID_RESULT_DTO"),
			TEXT("Result DTO revision metadata is invalid."));
	}
	OutResult.AbiVersion = static_cast<uint32>(Abi); OutResult.TerminalStatus = static_cast<EAssetDocumentRuntimeTerminalStatus>(Status);
	OutResult.FinalPhase = static_cast<EAssetDocumentRuntimeRequestPhase>(Phase); OutResult.RuntimeRevision = Revision; OutResult.CodecVersion = static_cast<uint32>(Codec);
	const bool bPayloadDecoded = DecodeOptionalCanonicalValue(Object, OutResult, OutDiagnostic)
		& DecodeResultBytes(Object, OutResult, OutDiagnostic)
		& DecodeResultDiagnostics(Object, OutResult, OutDiagnostic)
		& DecodeResultChangedSet(Object, OutResult, OutDiagnostic)
		& DecodeString(PyDict_GetItemString(Object, "semantic_hash"), OutResult.SemanticHash, OutDiagnostic);
	return bPayloadDecoded;
}
