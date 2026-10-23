// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentAIProviderContracts.h"

#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/SoftObjectPath.h"

#include <initializer_list>

namespace AIProviderContractsLocal
{
const FAssetDocumentRuntimeValueDto* FindExactField(
	const FAssetDocumentRuntimeValueDto& Object,
	const TCHAR* Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num())
	{
		return nullptr;
	}
	int32 FoundIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name)
		{
			if (FoundIndex != INDEX_NONE)
			{
				return nullptr;
			}
			FoundIndex = Index;
		}
	}
	return FoundIndex == INDEX_NONE ? nullptr : &Object.Elements[FoundIndex];
}

bool HasExactlyFields(
	const FAssetDocumentRuntimeValueDto& Object,
	std::initializer_list<const TCHAR*> Names)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()
		|| Object.ObjectKeys.Num() != static_cast<int32>(Names.size()))
	{
		return false;
	}
	TSet<FString> Seen;
	for (const FString& Key : Object.ObjectKeys)
	{
		if (Key.IsEmpty() || Seen.Contains(Key))
		{
			return false;
		}
		Seen.Add(Key);
	}
	for (const TCHAR* Name : Names)
	{
		if (FindExactField(Object, Name) == nullptr)
		{
			return false;
		}
	}
	return true;
}

bool IsNonEmptyString(const FAssetDocumentRuntimeValueDto* Value)
{
	return Value != nullptr
		&& Value->Kind == EAssetDocumentRuntimeValueKind::String
		&& !Value->StringValue.IsEmpty();
}

bool IsCanonicalObjectPath(const FAssetDocumentRuntimeValueDto* Value)
{
	if (!IsNonEmptyString(Value))
	{
		return false;
	}
	const FSoftObjectPath Path(Value->StringValue);
	return Path.IsValid()
		&& Path.GetSubPathString().IsEmpty()
		&& Path.ToString() == Value->StringValue;
}

bool IsStrictCanonicalValue(const FAssetDocumentRuntimeValueDto& Value)
{
	if (!ValidateAssetDocumentRuntimeValue(Value).bValid)
	{
		return false;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Number && !FMath::IsFinite(Value.NumberValue))
	{
		return false;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		for (const FAssetDocumentRuntimeValueDto& Child : Value.Elements)
		{
			if (!IsStrictCanonicalValue(Child)) return false;
		}
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		FString Previous;
		for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
		{
			const FString& Key = Value.ObjectKeys[Index];
			if (Key.IsEmpty() || (Index > 0 && Previous >= Key)
				|| !IsStrictCanonicalValue(Value.Elements[Index]))
			{
				return false;
			}
			Previous = Key;
		}
	}
	return true;
}
} // namespace

FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIPrimitiveFailure(
	const EAssetDocumentPrimitiveOutcomeStatus Status,
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = Status;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (Diagnostics.IsEmpty())
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Outcome.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Primitive");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	else
	{
		Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome NormalizeAssetDocumentAIPrimitiveOutcome(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FAssetDocumentPrimitiveOutcome Outcome)
{
	const FAssetDocumentContractValidationResult Validation =
		ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Outcome);
	if (!Validation.bValid)
	{
		return MakeAssetDocumentAIPrimitiveFailure(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("AIProvider.OperationRejected"),
			TEXT("The AI primitive outcome does not satisfy its frozen descriptor."),
			Validation.Diagnostics);
	}

	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		Outcome.Value = {};
		Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
		Outcome.HandleResults.Reset();
		Outcome.ChangedSet.Reset();
		if (Outcome.Diagnostics.IsEmpty())
		{
			return MakeAssetDocumentAIPrimitiveFailure(
				EAssetDocumentPrimitiveOutcomeStatus::Failed,
				TEXT("AIProvider.OperationRejected"),
				TEXT("A non-success AI primitive outcome requires a stable diagnostic."));
		}
		return Outcome;
	}
	return Outcome;
}

bool IsAssetDocumentAITransactionValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentAtomicTransaction* const ActiveTransaction)
{
	if (ActiveTransaction == nullptr)
	{
		return false;
	}

	const FAssetDocumentMutationToken& Token = ActiveTransaction->GetMutationToken();
	const EAssetDocumentAtomicState State = ActiveTransaction->GetState();
	return ActiveTransaction->GetTransactionId() == Invocation.TransactionId
		&& (State == EAssetDocumentAtomicState::Live
			|| State == EAssetDocumentAtomicState::Mutating)
		&& Token.RequestId == Invocation.RequestId
		&& Token.TransactionId == Invocation.TransactionId
		&& Token.RuntimeRevision == Invocation.RuntimeRevision;
}

bool IsAssetDocumentAICanonicalRuntimeValue(
	const FAssetDocumentRuntimeValueDto& Value)
{
	using namespace AIProviderContractsLocal;
	return IsStrictCanonicalValue(Value);
}

bool ParseAssetDocumentAIBlackboardKeyTypeRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const bool bRequireCanonicalProperties,
	FAssetDocumentAIBlackboardKeyTypeRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value))
	{
		return false;
	}
	const bool bHasExactFields = bRequireCanonicalProperties
		? HasExactlyFields(Value, {TEXT("blackboard_path"), TEXT("key_name"),
			TEXT("expected_class_path"), TEXT("canonical_properties")})
		: HasExactlyFields(Value, {TEXT("blackboard_path"), TEXT("key_name"),
			TEXT("expected_class_path")});
	if (!bHasExactFields)
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* BlackboardPath = FindExactField(Value, TEXT("blackboard_path"));
	const FAssetDocumentRuntimeValueDto* KeyName = FindExactField(Value, TEXT("key_name"));
	const FAssetDocumentRuntimeValueDto* ClassPath = FindExactField(Value, TEXT("expected_class_path"));
	if (!IsCanonicalObjectPath(BlackboardPath) || !IsNonEmptyString(KeyName)
		|| !IsCanonicalObjectPath(ClassPath))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* Properties = bRequireCanonicalProperties
		? FindExactField(Value, TEXT("canonical_properties")) : nullptr;
	if (bRequireCanonicalProperties && (Properties == nullptr
		|| Properties->Kind != EAssetDocumentRuntimeValueKind::Object
		|| Properties->ObjectKeys.Num() != Properties->Elements.Num()))
	{
		return false;
	}
	OutRequest.BlackboardPath = BlackboardPath->StringValue;
	OutRequest.KeyName = KeyName->StringValue;
	OutRequest.ExpectedClassPath = ClassPath->StringValue;
	OutRequest.CanonicalProperties = Properties;
	return true;
}

bool ParseAssetDocumentAIBlackboardEntryReadbackRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryReadbackRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasExactlyFields(Value, {TEXT("blackboard_path"), TEXT("key_name")}))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* BlackboardPath =
		FindExactField(Value, TEXT("blackboard_path"));
	const FAssetDocumentRuntimeValueDto* KeyName =
		FindExactField(Value, TEXT("key_name"));
	if (!IsCanonicalObjectPath(BlackboardPath) || !IsNonEmptyString(KeyName))
	{
		return false;
	}
	OutRequest.BlackboardPath = BlackboardPath->StringValue;
	OutRequest.KeyName = KeyName->StringValue;
	return true;
}

bool ParseAssetDocumentAIBlackboardParentReadbackRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardParentReadbackRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	const FAssetDocumentRuntimeValueDto* BlackboardPath = FindExactField(Value, TEXT("blackboard_path"));
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasExactlyFields(Value, {TEXT("blackboard_path")})
		|| !IsCanonicalObjectPath(BlackboardPath)) return false;
	OutRequest.BlackboardPath = BlackboardPath->StringValue;
	return true;
}

bool ParseAssetDocumentAIBlackboardLocalKeysReadbackRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardLocalKeysReadbackRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	const FAssetDocumentRuntimeValueDto* BlackboardPath = FindExactField(Value, TEXT("blackboard_path"));
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasExactlyFields(Value, {TEXT("blackboard_path")})
		|| !IsCanonicalObjectPath(BlackboardPath)) return false;
	OutRequest.BlackboardPath = BlackboardPath->StringValue;
	return true;
}

EAssetDocumentAIBlackboardEntryUpdateParseResult
ParseAssetDocumentAIBlackboardEntryUpdateExistingRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryUpdateExistingRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys != TArray<FString>{TEXT("blackboard_path"), TEXT("entry"), TEXT("key_name")}
		|| Value.Elements.Num() != 3
		|| !IsCanonicalObjectPath(&Value.Elements[0])
		|| Value.Elements[1].Kind != EAssetDocumentRuntimeValueKind::Object
		|| !IsNonEmptyString(&Value.Elements[2]))
	{
		return EAssetDocumentAIBlackboardEntryUpdateParseResult::InvalidEnvelope;
	}

	const FAssetDocumentRuntimeValueDto& Entry = Value.Elements[1];
	if (Entry.ObjectKeys != TArray<FString>{TEXT("Category"), TEXT("Description"),
			TEXT("KeyTypeClass"), TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")}
		|| Entry.Elements.Num() != 6
		|| Entry.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[2].Kind != EAssetDocumentRuntimeValueKind::Object
		|| Entry.Elements[2].ObjectKeys != TArray<FString>{TEXT("Kind"), TEXT("Path")}
		|| Entry.Elements[2].Elements.Num() != 2
		|| Entry.Elements[2].Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[2].Elements[0].StringValue != TEXT("ClassRef")
		|| !IsCanonicalObjectPath(&Entry.Elements[2].Elements[1])
		|| Entry.Elements[3].Kind != EAssetDocumentRuntimeValueKind::Object
		|| Entry.Elements[3].ObjectKeys.Num() != Entry.Elements[3].Elements.Num()
		|| !IsAssetDocumentAICanonicalRuntimeValue(Entry.Elements[3])
		|| !IsNonEmptyString(&Entry.Elements[4])
		|| Entry.Elements[4].StringValue != Value.Elements[2].StringValue
		|| Entry.Elements[5].Kind != EAssetDocumentRuntimeValueKind::Boolean)
	{
		return EAssetDocumentAIBlackboardEntryUpdateParseResult::InvalidEntry;
	}

	OutRequest.BlackboardPath = Value.Elements[0].StringValue;
	OutRequest.KeyName = Value.Elements[2].StringValue;
	OutRequest.ExpectedClassPath = Entry.Elements[2].Elements[1].StringValue;
	OutRequest.CanonicalProperties = Entry.Elements[3];
	OutRequest.Description = Entry.Elements[1].StringValue;
	OutRequest.Category = Entry.Elements[0].StringValue;
	OutRequest.bInstanceSynced = Entry.Elements[5].BooleanValue;
	return EAssetDocumentAIBlackboardEntryUpdateParseResult::Succeeded;
}

EAssetDocumentAIBlackboardEntryAddParseResult
ParseAssetDocumentAIBlackboardEntryAddRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryAddRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys != TArray<FString>{TEXT("blackboard_path"), TEXT("entry"), TEXT("insertion")}
		|| Value.Elements.Num() != 3
		|| !IsCanonicalObjectPath(&Value.Elements[0]))
	{
		return EAssetDocumentAIBlackboardEntryAddParseResult::InvalidEnvelope;
	}
	const FAssetDocumentRuntimeValueDto& Insertion = Value.Elements[2];
	if (Insertion.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Insertion.ObjectKeys != TArray<FString>{TEXT("Index"), TEXT("Kind")}
		|| Insertion.Elements.Num() != 2
		|| Insertion.Elements[0].Kind != EAssetDocumentRuntimeValueKind::Integer
		|| Insertion.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String
		|| Insertion.Elements[1].StringValue != TEXT("AtIndex"))
	{
		return EAssetDocumentAIBlackboardEntryAddParseResult::InvalidEnvelope;
	}
	const FAssetDocumentRuntimeValueDto& Entry = Value.Elements[1];
	if (Entry.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Entry.ObjectKeys != TArray<FString>{TEXT("Category"), TEXT("Description"),
			TEXT("KeyTypeClass"), TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")}
		|| Entry.Elements.Num() != 6
		|| Entry.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[2].Kind != EAssetDocumentRuntimeValueKind::Object
		|| Entry.Elements[2].ObjectKeys != TArray<FString>{TEXT("Kind"), TEXT("Path")}
		|| Entry.Elements[2].Elements.Num() != 2
		|| Entry.Elements[2].Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[2].Elements[0].StringValue != TEXT("ClassRef")
		|| !IsCanonicalObjectPath(&Entry.Elements[2].Elements[1])
		|| Entry.Elements[3].Kind != EAssetDocumentRuntimeValueKind::Object
		|| Entry.Elements[3].ObjectKeys.Num() != Entry.Elements[3].Elements.Num()
		|| !IsAssetDocumentAICanonicalRuntimeValue(Entry.Elements[3])
		|| !IsNonEmptyString(&Entry.Elements[4])
		|| Entry.Elements[5].Kind != EAssetDocumentRuntimeValueKind::Boolean
		|| Insertion.Elements[0].IntegerValue < 0
		|| Insertion.Elements[0].IntegerValue > MAX_int32)
	{
		return EAssetDocumentAIBlackboardEntryAddParseResult::InvalidEntry;
	}
	OutRequest.BlackboardPath = Value.Elements[0].StringValue;
	OutRequest.KeyName = Entry.Elements[4].StringValue;
	OutRequest.ExpectedClassPath = Entry.Elements[2].Elements[1].StringValue;
	OutRequest.CanonicalProperties = Entry.Elements[3];
	OutRequest.Description = Entry.Elements[1].StringValue;
	OutRequest.Category = Entry.Elements[0].StringValue;
	OutRequest.bInstanceSynced = Entry.Elements[5].BooleanValue;
	OutRequest.InsertionIndex = static_cast<int32>(Insertion.Elements[0].IntegerValue);
	return EAssetDocumentAIBlackboardEntryAddParseResult::Succeeded;
}

EAssetDocumentAIBlackboardEntryReplaceClassParseResult
ParseAssetDocumentAIBlackboardEntryReplaceClassRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryReplaceClassRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys != TArray<FString>{TEXT("blackboard_path"), TEXT("entry"), TEXT("key_name")}
		|| Value.Elements.Num() != 3
		|| !IsCanonicalObjectPath(&Value.Elements[0])
		|| Value.Elements[1].Kind != EAssetDocumentRuntimeValueKind::Object
		|| !IsNonEmptyString(&Value.Elements[2]))
	{
		return EAssetDocumentAIBlackboardEntryReplaceClassParseResult::InvalidEnvelope;
	}
	const FAssetDocumentRuntimeValueDto& Entry = Value.Elements[1];
	if (Entry.ObjectKeys != TArray<FString>{TEXT("Category"), TEXT("Description"),
			TEXT("KeyTypeClass"), TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")}
		|| Entry.Elements.Num() != 6
		|| Entry.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[2].Kind != EAssetDocumentRuntimeValueKind::Object
		|| Entry.Elements[2].ObjectKeys != TArray<FString>{TEXT("Kind"), TEXT("Path")}
		|| Entry.Elements[2].Elements.Num() != 2
		|| Entry.Elements[2].Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
		|| Entry.Elements[2].Elements[0].StringValue != TEXT("ClassRef")
		|| !IsCanonicalObjectPath(&Entry.Elements[2].Elements[1])
		|| Entry.Elements[3].Kind != EAssetDocumentRuntimeValueKind::Object
		|| Entry.Elements[3].ObjectKeys.Num() != Entry.Elements[3].Elements.Num()
		|| !IsAssetDocumentAICanonicalRuntimeValue(Entry.Elements[3])
		|| !IsNonEmptyString(&Entry.Elements[4])
		|| Entry.Elements[4].StringValue != Value.Elements[2].StringValue
		|| Entry.Elements[5].Kind != EAssetDocumentRuntimeValueKind::Boolean)
	{
		return EAssetDocumentAIBlackboardEntryReplaceClassParseResult::InvalidEntry;
	}
	OutRequest.BlackboardPath = Value.Elements[0].StringValue;
	OutRequest.KeyName = Value.Elements[2].StringValue;
	OutRequest.ExpectedClassPath = Entry.Elements[2].Elements[1].StringValue;
	OutRequest.CanonicalProperties = Entry.Elements[3];
	OutRequest.Description = Entry.Elements[1].StringValue;
	OutRequest.Category = Entry.Elements[0].StringValue;
	OutRequest.bInstanceSynced = Entry.Elements[5].BooleanValue;
	return EAssetDocumentAIBlackboardEntryReplaceClassParseResult::Succeeded;
}

bool ParseAssetDocumentAIBlackboardParentRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardParentRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value))
	{
		return false;
	}
	if (!HasExactlyFields(Value, {TEXT("blackboard_path"), TEXT("parent_path_or_null")}))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* BlackboardPath = FindExactField(Value, TEXT("blackboard_path"));
	const FAssetDocumentRuntimeValueDto* ParentPath = FindExactField(Value, TEXT("parent_path_or_null"));
	if (!IsCanonicalObjectPath(BlackboardPath) || ParentPath == nullptr
		|| (ParentPath->Kind != EAssetDocumentRuntimeValueKind::Null
			&& !IsCanonicalObjectPath(ParentPath)))
	{
		return false;
	}
	OutRequest.BlackboardPath = BlackboardPath->StringValue;
	OutRequest.bClearParent = ParentPath->Kind == EAssetDocumentRuntimeValueKind::Null;
	OutRequest.ParentPath = OutRequest.bClearParent ? FString() : ParentPath->StringValue;
	return true;
}

bool ParseAssetDocumentAIBlackboardLocalKeysRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardLocalKeysRequest& OutRequest)
{
	using namespace AIProviderContractsLocal;
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value))
	{
		return false;
	}
	if (!HasExactlyFields(Value, {TEXT("blackboard_path"), TEXT("ordered_keys")}))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* BlackboardPath = FindExactField(Value, TEXT("blackboard_path"));
	const FAssetDocumentRuntimeValueDto* OrderedKeys = FindExactField(Value, TEXT("ordered_keys"));
	if (!IsCanonicalObjectPath(BlackboardPath) || OrderedKeys == nullptr
		|| OrderedKeys->Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return false;
	}
	OutRequest.BlackboardPath = BlackboardPath->StringValue;
	OutRequest.OrderedKeys.Reset();
	OutRequest.OrderedKeys.Reserve(OrderedKeys->Elements.Num());
	for (const FAssetDocumentRuntimeValueDto& Row : OrderedKeys->Elements)
	{
		if (!HasExactlyFields(Row, {TEXT("name"), TEXT("key_type_class_path"), TEXT("canonical_properties")}))
		{
			return false;
		}
		const FAssetDocumentRuntimeValueDto* KeyName = FindExactField(Row, TEXT("name"));
		const FAssetDocumentRuntimeValueDto* ClassPath = FindExactField(Row, TEXT("key_type_class_path"));
		const FAssetDocumentRuntimeValueDto* Properties = FindExactField(Row, TEXT("canonical_properties"));
		if (!IsNonEmptyString(KeyName) || !IsCanonicalObjectPath(ClassPath)
			|| Properties == nullptr || Properties->Kind != EAssetDocumentRuntimeValueKind::Object
			|| Properties->ObjectKeys.Num() != Properties->Elements.Num())
		{
			return false;
		}
		FAssetDocumentAIBlackboardLocalKeyRequest& Parsed = OutRequest.OrderedKeys.AddDefaulted_GetRef();
		Parsed.KeyName = KeyName->StringValue;
		Parsed.KeyTypeClassPath = ClassPath->StringValue;
		Parsed.CanonicalProperties = Properties;
	}
	return true;
}

FAssetDocumentVerifiedLeaseResult AcquireAssetDocumentAIMutableAssetLease(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FAssetDocumentVerifiedLeaseResult Result;
	const FAssetDocumentPrimitiveHandleArgument* AssetArgument = nullptr;
	for (const FAssetDocumentPrimitiveHandleArgument& Argument : Invocation.HandleArguments)
	{
		if (Argument.Name == TEXT("asset"))
		{
			if (AssetArgument != nullptr)
			{
				return Result;
			}
			AssetArgument = &Argument;
		}
	}
	if (AssetArgument == nullptr)
	{
		return Result;
	}
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("ai");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Asset;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Handles.AcquireVerifiedLease(AssetArgument->Handle, Context);
}
