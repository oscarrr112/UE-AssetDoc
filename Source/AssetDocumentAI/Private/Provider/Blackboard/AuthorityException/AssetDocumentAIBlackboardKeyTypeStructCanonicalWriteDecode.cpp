// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"

#include "Provider/AssetDocumentAIProviderContracts.h"

#include "Runtime/AssetDocumentAtomicTransaction.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/BlackboardData.h"
#include "Editor.h"
#include "Internationalization/Text.h"
#include "Misc/PackageName.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/EnumProperty.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace CanonicalRead = AssetDocumentAIBlackboardKeyTypeStructCanonicalRead;

namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite
{
#if WITH_DEV_AUTOMATION_TESTS
namespace
{
EPrepareAuditTerminal GPrepareAuditTerminal = EPrepareAuditTerminal::NotEntered;
TArray<FDependencyAttemptAudit> GDependencyAttemptAudit;
int32 GScratchInitializeAuditCount = 0;
FPrepareTestControl GPrepareTestControl;
}

FScopedPrepareTestControl::FScopedPrepareTestControl(const FPrepareTestControl& Control)
	: Previous(GPrepareTestControl)
{
	GPrepareTestControl = Control;
}

FScopedPrepareTestControl::~FScopedPrepareTestControl()
{
	GPrepareTestControl = Previous;
}

void ResetPrepareAudit()
{
	GPrepareAuditTerminal = EPrepareAuditTerminal::NotEntered;
}

EPrepareAuditTerminal GetPrepareAuditTerminal()
{
	return GPrepareAuditTerminal;
}

const TCHAR* GetPrepareAuditTerminalName(const EPrepareAuditTerminal Terminal)
{
	switch (Terminal)
	{
	case EPrepareAuditTerminal::NotEntered: return TEXT("NotEntered");
	case EPrepareAuditTerminal::InvalidContext: return TEXT("InvalidContext");
	case EPrepareAuditTerminal::RootDependencyUnavailable: return TEXT("RootDependencyUnavailable");
	case EPrepareAuditTerminal::ScratchDecodeFailed: return TEXT("ScratchDecodeFailed");
	case EPrepareAuditTerminal::SchemaCaptureFailed: return TEXT("SchemaCaptureFailed");
	case EPrepareAuditTerminal::DetachedReadFailed: return TEXT("DetachedReadFailed");
	case EPrepareAuditTerminal::DetachedReadMismatch: return TEXT("DetachedReadMismatch");
	case EPrepareAuditTerminal::Prepared: return TEXT("Prepared");
	default: return TEXT("Unknown");
	}
}

void ResetDependencyAttemptAudit()
{
	GDependencyAttemptAudit.Reset();
}

TArray<FDependencyAttemptAudit> GetDependencyAttemptAudit()
{
	return GDependencyAttemptAudit;
}

void ResetScratchInitializeAudit()
{
	GScratchInitializeAuditCount = 0;
}

int32 GetScratchInitializeAuditCount()
{
	return GScratchInitializeAuditCount;
}
#endif

class FPreparedWriteState
{
public:
	ECaller Caller = ECaller::UpdateExistingV2;
	UBlackboardData* Blackboard = nullptr;
	UBlackboardKeyType_Struct* Target = nullptr;
	IAssetDocumentAtomicTransaction* Transaction = nullptr;
	FGuid TransactionId;
	FGuid RequestId;
	uint64 RuntimeRevision = 0;
	FInstancedStruct Scratch;
	FAssetDocumentRuntimeValueDto Desired;
	CanonicalRead::Private::FSchemaSnapshot Schema;
	TMap<FString, TStrongObjectPtr<UObject>> Dependencies;
};

FPreparedWrite::FPreparedWrite() = default;
FPreparedWrite::~FPreparedWrite() = default;
FPreparedWrite::FPreparedWrite(FPreparedWrite&&) noexcept = default;
FPreparedWrite& FPreparedWrite::operator=(FPreparedWrite&&) noexcept = default;
bool FPreparedWrite::IsValid() const { return State.IsValid(); }
ECaller FPreparedWrite::GetCaller() const { return State->Caller; }
UBlackboardData* FPreparedWrite::GetBlackboard() const { return State->Blackboard; }
UBlackboardKeyType_Struct* FPreparedWrite::GetExistingTarget() const { return State->Target; }
IAssetDocumentAtomicTransaction* FPreparedWrite::GetTransaction() const { return State->Transaction; }
FGuid FPreparedWrite::GetTransactionId() const { return State->TransactionId; }
FGuid FPreparedWrite::GetRequestId() const { return State->RequestId; }
uint64 FPreparedWrite::GetRuntimeRevision() const { return State->RuntimeRevision; }
const FInstancedStruct& FPreparedWrite::GetScratch() const { return State->Scratch; }
const FAssetDocumentRuntimeValueDto& FPreparedWrite::GetDesired() const { return State->Desired; }
const CanonicalRead::Private::FSchemaSnapshot& FPreparedWrite::GetSchema() const { return State->Schema; }

namespace
{
constexpr int32 MaxDepth = 16, MaxFields = 256, MaxContainer = 1024, MaxNodes = 4096, MaxPaths = 256;
constexpr int64 MaxTextBytes = 256 * 1024, MaxComparisonBytes = 1024 * 1024;
struct FContext { int32 Fields, Containers, Nodes; int64 ComparisonBytes; EFailure Failure; FPreparedWriteState& State; bool bMaterialize, bAllowDependencyResolve; };
bool Fail(FContext& C, EFailure F) { if (C.Failure == EFailure::None) C.Failure = F; return false; }
bool Consume(FContext& C, int32& Budget, int32 Amount = 1) { return Amount >= 0 && Budget >= Amount ? (Budget -= Amount, true) : Fail(C, EFailure::DecodeFailed); }
bool IsObject(const FAssetDocumentRuntimeValueDto& V, TConstArrayView<FString> Keys)
{ return V.Kind == EAssetDocumentRuntimeValueKind::Object && V.ObjectKeys == Keys && V.Elements.Num() == Keys.Num(); }
int32 TextBytes(const FString& S) { return FTCHARToUTF8_Convert::ConvertedLength(*S, S.Len()); }
bool CheckText(FContext& C, const FString& S)
{ const int32 Bytes = TextBytes(S); return Bytes >= 0 && Bytes <= MaxTextBytes ? true : Fail(C, EFailure::InvalidDto); }
bool MeasureCanonical(FContext& C, const FAssetDocumentRuntimeValueDto& V)
{
	auto Take = [&C](const int64 Amount) { return Amount >= 0 && C.ComparisonBytes >= Amount ? (C.ComparisonBytes -= Amount, true) : Fail(C, EFailure::DecodeFailed); };
	if (!Take(1)) return false;
	switch (V.Kind)
	{
	case EAssetDocumentRuntimeValueKind::Null: return true;
	case EAssetDocumentRuntimeValueKind::Boolean: return Take(1);
	case EAssetDocumentRuntimeValueKind::Integer:
	case EAssetDocumentRuntimeValueKind::Number: return Take(8);
	case EAssetDocumentRuntimeValueKind::String: return CheckText(C, V.StringValue) && Take(8 + TextBytes(V.StringValue));
	case EAssetDocumentRuntimeValueKind::Array:
		if (!Take(8)) return false;
		for (const auto& E : V.Elements) if (!MeasureCanonical(C, E)) return false;
		return true;
	case EAssetDocumentRuntimeValueKind::Object:
		if (V.ObjectKeys.Num() != V.Elements.Num() || !Take(8)) return Fail(C, EFailure::InvalidDto);
		for (int32 I = 0; I < V.Elements.Num(); ++I) if (!CheckText(C, V.ObjectKeys[I]) || !Take(9 + TextBytes(V.ObjectKeys[I])) || !MeasureCanonical(C, V.Elements[I])) return false;
		return true;
	default: return Fail(C, EFailure::InvalidDto);
	}
}
UObject* Resolve(FContext& C, const FString& Path)
{
	if (const TStrongObjectPtr<UObject>* Existing = C.State.Dependencies.Find(Path)) return Existing->Get();
	if (!C.bAllowDependencyResolve) { Fail(C, EFailure::DependencyUnavailable); return nullptr; }
	#if WITH_DEV_AUTOMATION_TESTS
	const int32 DependencyAttemptPrecharge = GPrepareTestControl.DependencyAttemptPrecharge;
	#else
	constexpr int32 DependencyAttemptPrecharge = 0;
	#endif
	if (DependencyAttemptPrecharge + C.State.Dependencies.Num() >= MaxPaths)
	{
		Fail(C, EFailure::DependencyUnavailable);
		return nullptr;
	}
	const FSoftObjectPath Soft(Path);
	if (!Soft.IsValid() || Soft.GetSubPathString().IsEmpty() == false || Soft.ToString() != Path) { Fail(C, EFailure::DependencyUnavailable); return nullptr; }
	#if WITH_DEV_AUTOMATION_TESTS
	FDependencyAttemptAudit& Attempt = GDependencyAttemptAudit.AddDefaulted_GetRef();
	Attempt.LiteralPath = Path;
	#endif
	UObject* Object = Soft.ResolveObject();
	#if WITH_DEV_AUTOMATION_TESTS
	Attempt.bResolveObjectSucceeded = Object != nullptr;
	#endif
	if (Object == nullptr)
	{
		#if WITH_DEV_AUTOMATION_TESTS
		Attempt.bTriedLoad = true;
		#endif
		Object = Soft.TryLoad();
		#if WITH_DEV_AUTOMATION_TESTS
		Attempt.bLoadSucceeded = Object != nullptr;
		#endif
	}
	if (Object == nullptr || Object->GetPathName() != Path) { Fail(C, EFailure::DependencyUnavailable); return nullptr; }
	C.State.Dependencies.Add(Path, TStrongObjectPtr<UObject>(Object)); return Object;
}
bool Decode(FContext&, const FProperty&, void*, const FAssetDocumentRuntimeValueDto&, int32);
bool DecodeStruct(FContext& C, const UScriptStruct& Struct, void* Memory, const FAssetDocumentRuntimeValueDto& V, int32 Depth)
{
	if (!IsObject(V, {TEXT("Properties"), TEXT("Struct")}) || V.Elements[0].Kind != EAssetDocumentRuntimeValueKind::Object
		|| V.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String || Depth > MaxDepth || !CheckText(C, V.Elements[1].StringValue)) return Fail(C, EFailure::InvalidDto);
	UScriptStruct* Resolved = Cast<UScriptStruct>(Resolve(C, V.Elements[1].StringValue));
	if (Resolved != &Struct || !Consume(C, C.Nodes, 3)) return Fail(C, EFailure::SchemaDrift);
	TArray<TPair<FString, const FProperty*>> Fields; CanonicalRead::EFailure ReadFailure = CanonicalRead::EFailure::None;
	if (!CanonicalRead::Private::CollectStoredFields(Struct, C.Fields, Fields, ReadFailure) || Fields.Num() != V.Elements[0].Elements.Num() || Fields.Num() != V.Elements[0].ObjectKeys.Num()) return Fail(C, EFailure::DecodeFailed);
	for (int32 I = 0; I < Fields.Num(); ++I)
	{
		if (Fields[I].Key != V.Elements[0].ObjectKeys[I] || !CheckText(C, Fields[I].Key)) return Fail(C, EFailure::InvalidDto);
		const FProperty& P = *Fields[I].Value;
		if (P.ArrayDim > 1)
		{
			const FAssetDocumentRuntimeValueDto& A = V.Elements[0].Elements[I];
			if (A.Kind != EAssetDocumentRuntimeValueKind::Array || A.Elements.Num() != P.ArrayDim || !Consume(C, C.Containers, P.ArrayDim)) return Fail(C, EFailure::InvalidDto);
			for (int32 J = 0; J < P.ArrayDim; ++J) if (!Decode(C, P, C.bMaterialize ? P.ContainerPtrToValuePtr<void>(Memory, J) : nullptr, A.Elements[J], Depth + 1)) return false;
		}
		else if (!Decode(C, P, C.bMaterialize ? P.ContainerPtrToValuePtr<void>(Memory) : nullptr, V.Elements[0].Elements[I], Depth + 1)) return false;
	}
	return true;
}
bool DecodeRef(FContext& C, const FProperty& P, void* A, const FAssetDocumentRuntimeValueDto& V, const TCHAR* Kind)
{
	if (V.Kind == EAssetDocumentRuntimeValueKind::Null) { if (C.bMaterialize) CastFieldChecked<FObjectPropertyBase>(&P)->SetObjectPropertyValue(A, nullptr); return Consume(C, C.Nodes); }
	if (!IsObject(V, {TEXT("Kind"), TEXT("Path")}) || V.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String || V.Elements[0].StringValue != Kind || V.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String) return Fail(C, EFailure::InvalidDto);
	UObject* O = Resolve(C, V.Elements[1].StringValue); const FObjectPropertyBase* OP = CastFieldChecked<FObjectPropertyBase>(&P);
	if (O == nullptr || !O->IsA(OP->PropertyClass)) return Fail(C, EFailure::DependencyUnavailable);
	if (const FClassProperty* CP = CastField<FClassProperty>(&P)) { UClass* Class = Cast<UClass>(O); if (Class == nullptr || !Class->IsChildOf(CP->MetaClass)) return Fail(C, EFailure::DependencyUnavailable); }
	if (C.bMaterialize) OP->SetObjectPropertyValue(A, O); return Consume(C, C.Nodes, 3);
}
bool DecodeContainer(FContext& C, const FProperty& P, void* A, const FAssetDocumentRuntimeValueDto& V, int32 D)
{
	if (V.Kind != EAssetDocumentRuntimeValueKind::Array || V.Elements.Num() > MaxContainer || !Consume(C, C.Containers, V.Elements.Num())) return Fail(C, EFailure::InvalidDto);
	if (const FArrayProperty* AP = CastField<FArrayProperty>(&P))
	{
		if (!C.bMaterialize) { for (const auto& E : V.Elements) if (!Decode(C, *AP->Inner, nullptr, E, D + 1)) return false; return Consume(C, C.Nodes); }
		FScriptArrayHelper H(AP, A); H.EmptyValues(V.Elements.Num());
		for (const auto& E : V.Elements) { const int32 I = H.AddValue(); if (!Decode(C, *AP->Inner, H.GetElementPtr(I), E, D + 1)) return false; } return Consume(C, C.Nodes);
	}
	if (const FSetProperty* SP = CastField<FSetProperty>(&P))
	{
		if (!SP->ElementProp->HasAnyPropertyFlags(CPF_HasGetValueTypeHash)) return Fail(C, EFailure::DecodeFailed);
		if (!C.bMaterialize) { for (const auto& E : V.Elements) if (!MeasureCanonical(C, E) || !Decode(C, *SP->ElementProp, nullptr, E, D + 1)) return false; return Consume(C, C.Nodes); }
		FScriptSetHelper H(SP, A); H.EmptyElements(V.Elements.Num());
		for (const auto& E : V.Elements) { const int32 I = H.AddDefaultValue_Invalid_NeedsRehash(); if (!Decode(C, *SP->ElementProp, H.GetElementPtr(I), E, D + 1)) return false; } H.Rehash(); return H.Num() == V.Elements.Num() ? Consume(C, C.Nodes) : Fail(C, EFailure::DecodeFailed);
	}
	const FMapProperty* MP = CastField<FMapProperty>(&P); if (MP == nullptr || !MP->KeyProp->HasAnyPropertyFlags(CPF_HasGetValueTypeHash)) return Fail(C, EFailure::DecodeFailed);
	if (!C.bMaterialize) { for (const auto& E : V.Elements) { if (!IsObject(E, {TEXT("Key"), TEXT("Value")}) || !MeasureCanonical(C, E.Elements[0]) || !Decode(C, *MP->KeyProp, nullptr, E.Elements[0], D + 1) || !Decode(C, *MP->ValueProp, nullptr, E.Elements[1], D + 1)) return false; } return Consume(C, C.Nodes, V.Elements.Num() + 1); }
	FScriptMapHelper H(MP, A); H.EmptyValues(V.Elements.Num());
	for (const auto& E : V.Elements) { if (!IsObject(E, {TEXT("Key"), TEXT("Value")})) return Fail(C, EFailure::InvalidDto); const int32 I = H.AddDefaultValue_Invalid_NeedsRehash(); if (!Decode(C, *MP->KeyProp, H.GetKeyPtr(I), E.Elements[0], D + 1) || !Decode(C, *MP->ValueProp, H.GetValuePtr(I), E.Elements[1], D + 1)) return false; }
	H.Rehash(); return H.Num() == V.Elements.Num() ? Consume(C, C.Nodes, V.Elements.Num() + 1) : Fail(C, EFailure::DecodeFailed);
}
bool Decode(FContext& C, const FProperty& P, void* A, const FAssetDocumentRuntimeValueDto& V, int32 D)
{
	if ((C.bMaterialize && A == nullptr) || D > MaxDepth) return Fail(C, EFailure::DecodeFailed);
	if (CastField<FArrayProperty>(&P) || CastField<FSetProperty>(&P) || CastField<FMapProperty>(&P)) return DecodeContainer(C, P, A, V, D);
	if (const FStructProperty* SP = CastField<FStructProperty>(&P))
	{
		if (SP->Struct == FInstancedStruct::StaticStruct())
		{
			if (V.Kind == EAssetDocumentRuntimeValueKind::Null) { if (C.bMaterialize) static_cast<FInstancedStruct*>(A)->Reset(); return Consume(C, C.Nodes); }
			if (!IsObject(V, {TEXT("Properties"), TEXT("Struct")}) || V.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String) return Fail(C, EFailure::InvalidDto);
			UScriptStruct* S = Cast<UScriptStruct>(Resolve(C, V.Elements[1].StringValue));
			if (S == nullptr) return Fail(C, EFailure::DependencyUnavailable);
			if (!C.bMaterialize) return DecodeStruct(C, *S, nullptr, V, D);
			FInstancedStruct& Dynamic = *static_cast<FInstancedStruct*>(A);
			#if WITH_DEV_AUTOMATION_TESTS
			++GScratchInitializeAuditCount;
			#endif
			Dynamic.InitializeAs(S); return DecodeStruct(C, *S, Dynamic.GetMutableMemory(), V, D);
		}
		return SP->Struct != nullptr ? DecodeStruct(C, *SP->Struct, A, V, D) : Fail(C, EFailure::DecodeFailed);
	}
	if (const FBoolProperty* BP = CastField<FBoolProperty>(&P)) { if (V.Kind != EAssetDocumentRuntimeValueKind::Boolean) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) BP->SetPropertyValue(A, V.BooleanValue); return Consume(C, C.Nodes); }
	if (const FStrProperty* SP = CastField<FStrProperty>(&P)) { if (V.Kind != EAssetDocumentRuntimeValueKind::String || !CheckText(C, V.StringValue)) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) SP->SetPropertyValue(A, V.StringValue); return Consume(C, C.Nodes); }
	if (const FNameProperty* NP = CastField<FNameProperty>(&P)) { if (V.Kind != EAssetDocumentRuntimeValueKind::String || !CheckText(C, V.StringValue)) return Fail(C, EFailure::InvalidDto); const FName Name = V.StringValue.IsEmpty() ? NAME_None : FName(*V.StringValue); if (!V.StringValue.IsEmpty() && Name.ToString() != V.StringValue) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) NP->SetPropertyValue(A, Name); return Consume(C, C.Nodes); }
	if (const FTextProperty* TP = CastField<FTextProperty>(&P)) { if (V.Kind != EAssetDocumentRuntimeValueKind::String || !CheckText(C, V.StringValue)) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) TP->SetPropertyValue(A, FText::AsCultureInvariant(V.StringValue)); return Consume(C, C.Nodes); }
	if (const FSoftClassProperty* SCP = CastField<FSoftClassProperty>(&P)) { if (V.Kind == EAssetDocumentRuntimeValueKind::Null) { if (C.bMaterialize) SCP->SetPropertyValue(A, FSoftObjectPtr()); return Consume(C, C.Nodes); } if (!IsObject(V, {TEXT("Kind"), TEXT("Path")}) || V.Elements[0].StringValue != TEXT("SoftClassRef") || V.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String) return Fail(C, EFailure::InvalidDto); FSoftObjectPath Path(V.Elements[1].StringValue); if (!Path.IsValid() || Path.ToString() != V.Elements[1].StringValue) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) SCP->SetPropertyValue(A, FSoftObjectPtr(Path)); return Consume(C, C.Nodes, 3); }
	if (const FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(&P)) { if (V.Kind == EAssetDocumentRuntimeValueKind::Null) { if (C.bMaterialize) SOP->SetPropertyValue(A, FSoftObjectPtr()); return Consume(C, C.Nodes); } if (!IsObject(V, {TEXT("Kind"), TEXT("Path")}) || V.Elements[0].StringValue != TEXT("SoftAssetRef") || V.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String) return Fail(C, EFailure::InvalidDto); FSoftObjectPath Path(V.Elements[1].StringValue); if (!Path.IsValid() || Path.ToString() != V.Elements[1].StringValue) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) SOP->SetPropertyValue(A, FSoftObjectPtr(Path)); return Consume(C, C.Nodes, 3); }
	if (CastField<FObjectProperty>(&P) || CastField<FClassProperty>(&P)) return DecodeRef(C, P, A, V, CastField<FClassProperty>(&P) ? TEXT("ClassRef") : TEXT("AssetRef"));
	const FNumericProperty* NP = CastField<FNumericProperty>(&P);
	if (const FEnumProperty* EP = CastField<FEnumProperty>(&P)) NP = EP->GetUnderlyingProperty();
	if (NP == nullptr) return Fail(C, EFailure::DecodeFailed);
	UEnum* Enum = CastField<FEnumProperty>(&P) ? CastField<FEnumProperty>(&P)->GetEnum() : NP->GetIntPropertyEnum();
	if (Enum != nullptr)
	{
		if (!IsObject(V, {TEXT("Enum"), TEXT("Enumerator")}) || V.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String || V.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String) return Fail(C, EFailure::InvalidDto);
		UEnum* ResolvedEnum = Cast<UEnum>(Resolve(C, V.Elements[0].StringValue));
		if (ResolvedEnum != Enum) return Fail(C, EFailure::DependencyUnavailable);
		const int64 N = Enum->GetValueByNameString(V.Elements[1].StringValue); const int32 Index = Enum->GetIndexByValue(N);
		if (Enum->HasAnyEnumFlags(EEnumFlags::Flags) || Index == INDEX_NONE || Enum->GetNameStringByValue(N) != V.Elements[1].StringValue
			|| V.Elements[1].StringValue == TEXT("MAX") || V.Elements[1].StringValue.EndsWith(TEXT("_MAX"))
			|| Enum->HasMetaData(TEXT("Hidden"), Index) || Enum->HasMetaData(TEXT("Spacer"), Index)) return Fail(C, EFailure::InvalidDto);
		if (C.bMaterialize) NP->SetIntPropertyValue(A, N);
		return Consume(C, C.Nodes, 3);
	}
	if (NP->IsFloatingPoint()) { if (V.Kind != EAssetDocumentRuntimeValueKind::Number || !FMath::IsFinite(V.NumberValue)) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) NP->SetFloatingPointPropertyValue(A, V.NumberValue == 0.0 ? 0.0 : V.NumberValue); return Consume(C, C.Nodes); }
	if (CastField<FUInt64Property>(&P)) { if (!IsObject(V, {TEXT("Kind"), TEXT("Value")}) || V.Elements[0].StringValue != TEXT("UInt64") || V.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String) return Fail(C, EFailure::InvalidDto); uint64 N = 0; if (!LexTryParseString(N, *V.Elements[1].StringValue) || LexToString(N) != V.Elements[1].StringValue) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) NP->SetIntPropertyValue(A, N); return Consume(C, C.Nodes, 3); }
	if (V.Kind != EAssetDocumentRuntimeValueKind::Integer) return Fail(C, EFailure::InvalidDto); if (C.bMaterialize) NP->SetIntPropertyValue(A, V.IntegerValue); return Consume(C, C.Nodes);
}
}

FPrepareResult PrepareCanonicalWrite(const ECaller Caller, UBlackboardData& Blackboard, const UClass& RequestedClass,
	UBlackboardKeyType_Struct* Target, IAssetDocumentAtomicTransaction& Transaction, const FAssetDocumentRuntimeValueDto& Desired)
{
	FPrepareResult Result;
	const bool bUpdateExisting = Caller == ECaller::UpdateExistingV2;
	const bool bStagedCaller = Caller == ECaller::AddV2 || Caller == ECaller::ReplaceClassV2;
	if ((!bUpdateExisting && !bStagedCaller)
		|| &RequestedClass != UBlackboardKeyType_Struct::StaticClass()
		|| (bUpdateExisting
			? Target == nullptr || Target->GetClass() != UBlackboardKeyType_Struct::StaticClass()
				|| Target->GetOuter() != &Blackboard
			: Target != nullptr))
	{
		#if WITH_DEV_AUTOMATION_TESTS
		GPrepareAuditTerminal = EPrepareAuditTerminal::InvalidContext;
		#endif
		Result.Failure = EFailure::InvalidContext;
		return Result;
	}
	if (!IsAssetDocumentAICanonicalRuntimeValue(Desired) || !IsObject(Desired, {TEXT("DefaultValue")}))
	{
		Result.Failure = EFailure::InvalidDto;
		return Result;
	}
	FPreparedWriteState& State = *(Result.Prepared.State = MakeUnique<FPreparedWriteState>()); State.Caller = Caller; State.Blackboard = &Blackboard; State.Target = Target; State.Transaction = &Transaction;
	const FAssetDocumentMutationToken& Token = Transaction.GetMutationToken();
	State.TransactionId = Transaction.GetTransactionId(); State.RequestId = Token.RequestId; State.RuntimeRevision = Token.RuntimeRevision;
	FContext Admission{MaxFields, MaxContainer, MaxNodes, MaxComparisonBytes, EFailure::None, State, false, true};
	#if WITH_DEV_AUTOMATION_TESTS
	Admission.Fields -= GPrepareTestControl.FieldsPrecharge;
	Admission.Containers -= GPrepareTestControl.ContainerPrecharge;
	Admission.Nodes -= GPrepareTestControl.NodesPrecharge;
	#endif
	const FAssetDocumentRuntimeValueDto& Value = Desired.Elements[0];
	UScriptStruct* RootStruct = nullptr;
	if (!MeasureCanonical(Admission, Value)) {}
	else if (Value.Kind == EAssetDocumentRuntimeValueKind::Null) {}
	else if (IsObject(Value, {TEXT("Properties"), TEXT("Struct")}) && Value.Elements[1].Kind == EAssetDocumentRuntimeValueKind::String)
	{
		RootStruct = Cast<UScriptStruct>(Resolve(Admission, Value.Elements[1].StringValue));
		if (RootStruct != nullptr) DecodeStruct(Admission, *RootStruct, nullptr, Value, 0);
		else
		{
			Fail(Admission, EFailure::DependencyUnavailable);
			#if WITH_DEV_AUTOMATION_TESTS
			GPrepareAuditTerminal = EPrepareAuditTerminal::RootDependencyUnavailable;
			#endif
		}
	}
	else Fail(Admission, EFailure::InvalidDto);
	if (Admission.Failure != EFailure::None)
	{
		#if WITH_DEV_AUTOMATION_TESTS
		if (GPrepareAuditTerminal != EPrepareAuditTerminal::RootDependencyUnavailable)
			GPrepareAuditTerminal = EPrepareAuditTerminal::ScratchDecodeFailed;
		#endif
		Result.Failure = Admission.Failure;
		Result.Prepared = FPreparedWrite();
		return Result;
	}
	FContext Materialize{MaxFields, MaxContainer, MaxNodes, MaxComparisonBytes, EFailure::None, State, true, false};
	if (RootStruct != nullptr)
	{
		UScriptStruct* CachedRoot = Cast<UScriptStruct>(Resolve(Materialize, Value.Elements[1].StringValue));
		if (CachedRoot != RootStruct) Fail(Materialize, EFailure::SchemaDrift);
		else
		{
		#if WITH_DEV_AUTOMATION_TESTS
		++GScratchInitializeAuditCount;
		#endif
		State.Scratch.InitializeAs(RootStruct); DecodeStruct(Materialize, *RootStruct, State.Scratch.GetMutableMemory(), Value, 0);
		}
	}
	if (Materialize.Failure != EFailure::None) { Result.Failure = Materialize.Failure; Result.Prepared = FPreparedWrite(); return Result; }
	const UScriptStruct* Struct = State.Scratch.GetScriptStruct(); const void* Memory = State.Scratch.GetMemory(); CanonicalRead::EFailure ReadFailure = CanonicalRead::EFailure::None;
	if (State.Scratch.IsValid() && (Struct == nullptr || Memory == nullptr
		|| !CanonicalRead::Private::CaptureSchemaSnapshot(*Struct, Memory, State.Schema, ReadFailure)))
	{
		#if WITH_DEV_AUTOMATION_TESTS
		GPrepareAuditTerminal = EPrepareAuditTerminal::SchemaCaptureFailed;
		#endif
		Result.Failure = EFailure::SchemaDrift;
		Result.Prepared = FPreparedWrite();
		return Result;
	}
	const CanonicalRead::FReadResult Read = CanonicalRead::ReadCanonicalValue(State.Scratch, CanonicalRead::ESource::DetachedScratch, nullptr);
	if (!Read.bSucceeded)
	{
		#if WITH_DEV_AUTOMATION_TESTS
		GPrepareAuditTerminal = EPrepareAuditTerminal::DetachedReadFailed;
		#endif
		Result.Failure = EFailure::ReadbackFailed;
		Result.Prepared = FPreparedWrite();
		return Result;
	}
	if (!AssetDocumentAIBlackboardMutation::RuntimeValueEquals(Read.Value, Value))
	{
		#if WITH_DEV_AUTOMATION_TESTS
		GPrepareAuditTerminal = EPrepareAuditTerminal::DetachedReadMismatch;
		#endif
		Result.Failure = EFailure::ReadbackFailed;
		Result.Prepared = FPreparedWrite();
		return Result;
	}
	#if WITH_DEV_AUTOMATION_TESTS
	GPrepareAuditTerminal = EPrepareAuditTerminal::Prepared;
	#endif
	State.Desired = Desired;
	Result.bSucceeded = true; Result.Failure = EFailure::None; return Result;
}
} // namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite
