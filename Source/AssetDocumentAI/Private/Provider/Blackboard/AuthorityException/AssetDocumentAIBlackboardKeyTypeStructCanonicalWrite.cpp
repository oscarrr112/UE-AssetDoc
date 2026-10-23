// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"

#include "Runtime/AssetDocumentAtomicTransaction.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/BlackboardData.h"
#include "Editor.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UnrealType.h"

namespace CanonicalRead = AssetDocumentAIBlackboardKeyTypeStructCanonicalRead;

namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite
{
namespace
{
#if WITH_DEV_AUTOMATION_TESTS
TArray<FLifecycleAuditEvent> GLifecycleAudit;
FLifecycleTestControl GLifecycleTestControl;
#endif

bool IsSafe(const UBlackboardKeyType_Struct& Target)
{
	return IsInGameThread() && GIsEditor && !IsRunningCommandlet() && !IsGarbageCollecting()
		&& Target.GetClass() == UBlackboardKeyType_Struct::StaticClass()
		&& !Target.HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed);
}

bool HasExactDefaultValueProperty()
{
	const FStructProperty* Property = FindFProperty<FStructProperty>(UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	return Property != nullptr && Property->GetOwnerStruct() == UBlackboardKeyType_Struct::StaticClass()
		&& Property->Struct == FInstancedStruct::StaticStruct();
}

}

#if WITH_DEV_AUTOMATION_TESTS
FScopedLifecycleTestControl::FScopedLifecycleTestControl(const FLifecycleTestControl& Control)
	: Previous(GLifecycleTestControl)
{
	GLifecycleTestControl = Control;
}

FScopedLifecycleTestControl::~FScopedLifecycleTestControl()
{
	GLifecycleTestControl = Previous;
}

void ResetLifecycleAudit()
{
	GLifecycleAudit.Reset();
}

TArray<FLifecycleAuditEvent> GetLifecycleAudit()
{
	return GLifecycleAudit;
}
#endif

FApplyResult ApplyPreparedCanonicalWrite(const FPreparedWrite& Prepared, const ECaller Caller,
	UBlackboardKeyType_Struct& Target, IAssetDocumentAtomicTransaction& Transaction)
{
	const bool bUpdateExisting = Caller == ECaller::UpdateExistingV2;
	const bool bStagedCaller = Caller == ECaller::AddV2 || Caller == ECaller::ReplaceClassV2;
	if (!Prepared.IsValid() || Prepared.GetCaller() != Caller || Prepared.GetBlackboard() == nullptr
		|| (!bUpdateExisting && !bStagedCaller)
		|| (bUpdateExisting ? Prepared.GetExistingTarget() != &Target : Prepared.GetExistingTarget() != nullptr)
		|| Prepared.GetTransaction() != &Transaction
		|| Transaction.GetTransactionId() != Prepared.GetTransactionId()
		|| Transaction.GetMutationToken().RequestId != Prepared.GetRequestId()
		|| Transaction.GetMutationToken().RuntimeRevision != Prepared.GetRuntimeRevision()
		|| Target.GetOuter() != Prepared.GetBlackboard() || !IsSafe(Target) || !HasExactDefaultValueProperty())
		return {false, EFailure::InvalidContext};
	if (!Prepared.GetDesired().Elements.IsValidIndex(0)) return {false, EFailure::InvalidContext};
	const FAssetDocumentRuntimeValueDto& Desired = Prepared.GetDesired().Elements[0];
	const FInstancedStruct& Scratch = Prepared.GetScratch();
	const bool bDesiredNull = Desired.Kind == EAssetDocumentRuntimeValueKind::Null;
	if (bDesiredNull ? Scratch.IsValid() : !Scratch.IsValid()) return {false, EFailure::SchemaDrift};
	if (!bDesiredNull)
	{
		const UScriptStruct* ScriptStruct = Scratch.GetScriptStruct();
		const void* Memory = Scratch.GetMemory();
		CanonicalRead::Private::FSchemaSnapshot Schema; CanonicalRead::EFailure SchemaFailure = CanonicalRead::EFailure::None;
		if (ScriptStruct == nullptr || Memory == nullptr
			|| !CanonicalRead::Private::CaptureSchemaSnapshot(*ScriptStruct, Memory, Schema, SchemaFailure)
			|| !(Schema == Prepared.GetSchema())) return {false, EFailure::SchemaDrift};
	}
	const CanonicalRead::FReadResult ScratchRead = CanonicalRead::ReadCanonicalValue(Scratch, CanonicalRead::ESource::DetachedScratch, nullptr);
	if (!ScratchRead.bSucceeded || !AssetDocumentAIBlackboardMutation::RuntimeValueEquals(
		ScratchRead.Value, Desired)) return {false, EFailure::ReadbackFailed};
	#if WITH_DEV_AUTOMATION_TESTS
	if (GLifecycleTestControl.FailurePoint == ELifecycleTestFailurePoint::BeforeLiveLifecycle)
		return {false, EFailure::ReadbackFailed};
	const bool bFailAfterAssignment = GLifecycleTestControl.FailurePoint == ELifecycleTestFailurePoint::AfterAssignment;
	#endif
	FStructProperty* Property = FindFProperty<FStructProperty>(UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	Target.PreEditChange(Property);
	Target.DefaultValue = Prepared.GetScratch();
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Event.SetActiveMemberProperty(Property);
	Target.PostEditChangeProperty(Event);
	#if WITH_DEV_AUTOMATION_TESTS
	GLifecycleAudit.Add({Caller, Property != nullptr, Event.MemberProperty == Property, true, true});
	if (bFailAfterAssignment) return {false, EFailure::ReadbackFailed};
	#endif
	const CanonicalRead::FReadResult LiveRead = CanonicalRead::ReadCanonicalValue(Target.DefaultValue, CanonicalRead::ESource::ExactLiveDefaultValue, &Target);
	return LiveRead.bSucceeded && AssetDocumentAIBlackboardMutation::RuntimeValueEquals(
		LiveRead.Value, Desired)
		? FApplyResult{true, EFailure::None} : FApplyResult{false, EFailure::ReadbackFailed};
}
} // namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite
