// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentAIBlackboardCanonicalStructTestTypes.h"
#include "AssetDocumentAIBlackboardConsumerCompletionTestTypes.h"
#include "AssetDocumentAITypedPreimageTestTypes.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AIGraphTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerCompletion.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalRead.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardPrimitives.h"
#include "AssetDocumentRollbackAction.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Engine/DataAsset.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "StructUtils/UserDefinedStruct.h"

#include <limits>
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace AssetDocumentAIBlackboardLifecycleTests
{
constexpr const TCHAR* KeyTypeInspectId = TEXT("ai.blackboard.key_type.inspect.v1");
constexpr const TCHAR* KeyTypeApplyId = TEXT("ai.blackboard.key_type.apply_properties.v1");
constexpr const TCHAR* ParentSetId = TEXT("ai.blackboard.parent.set.v1");
constexpr const TCHAR* LocalKeysReplaceId = TEXT("ai.blackboard.local_keys.replace.v1");
constexpr const TCHAR* EntryReadbackV2Id = TEXT("ai.blackboard.entry.readback.v2");
constexpr const TCHAR* ParentReadbackV2Id = TEXT("ai.blackboard.parent.readback.v2");
constexpr const TCHAR* ParentSetV2Id = TEXT("ai.blackboard.parent.set.v2");
constexpr const TCHAR* LocalKeysReadbackV2Id = TEXT("ai.blackboard.local_keys.readback.v2");
constexpr const TCHAR* EntryUpdateExistingV2Id = TEXT("ai.blackboard.entry.update_existing.v2");
constexpr const TCHAR* EntryAddV2Id = TEXT("ai.blackboard.entry.add.v2");
constexpr const TCHAR* EntryReplaceClassV2Id = TEXT("ai.blackboard.entry.replace_class.v2");
constexpr const TCHAR* EntryRemoveV2Id = TEXT("ai.blackboard.entry.remove.v2");
constexpr const TCHAR* LocalKeysReorderV2Id = TEXT("ai.blackboard.local_keys.reorder.v2");

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto IntegerValue(const int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto NumberValue(const double Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number;
	Result.NumberValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto BooleanValue(const bool Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto NullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto ReferenceValue(const TCHAR* Kind, const UObject& Object)
{
	return ObjectValue({TEXT("Kind"), TEXT("Path")},
		{StringValue(Kind), StringValue(Object.GetPathName())});
}

FAssetDocumentRuntimeValueDto ArrayValue(TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Values);
	return Result;
}

bool HasDiagnostic(const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Code)
{
	return Outcome.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentRuntimeDiagnostic& Item)
	{
		return Item.Code == Code;
	});
}

bool RuntimeValueEquals(
	const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right)
{
	if (Left.Kind != Right.Kind || Left.BooleanValue != Right.BooleanValue
		|| Left.IntegerValue != Right.IntegerValue || Left.NumberValue != Right.NumberValue
		|| Left.StringValue != Right.StringValue || Left.BytesValue != Right.BytesValue
		|| Left.ObjectKeys != Right.ObjectKeys || Left.Elements.Num() != Right.Elements.Num()) return false;
	for (int32 Index = 0; Index < Left.Elements.Num(); ++Index)
	{
		if (!RuntimeValueEquals(Left.Elements[Index], Right.Elements[Index])) return false;
	}
	return true;
}

const TCHAR* RuntimeValueKindName(const EAssetDocumentRuntimeValueKind Kind)
{
	switch (Kind)
	{
	case EAssetDocumentRuntimeValueKind::Null: return TEXT("Null");
	case EAssetDocumentRuntimeValueKind::Boolean: return TEXT("Boolean");
	case EAssetDocumentRuntimeValueKind::Integer: return TEXT("Integer");
	case EAssetDocumentRuntimeValueKind::Number: return TEXT("Number");
	case EAssetDocumentRuntimeValueKind::String: return TEXT("String");
	case EAssetDocumentRuntimeValueKind::Bytes: return TEXT("Bytes");
	case EAssetDocumentRuntimeValueKind::Array: return TEXT("Array");
	case EAssetDocumentRuntimeValueKind::Object: return TEXT("Object");
	default: return TEXT("Unknown");
	}
}

FString RuntimeValueAuditSummary(const FAssetDocumentRuntimeValueDto& Value)
{
	return FString::Printf(
		TEXT("kind=%s bool=%s integer=%lld number=%.17g string='%s' bytes=%d object_keys=[%s] elements=%d"),
		RuntimeValueKindName(Value.Kind), Value.BooleanValue ? TEXT("true") : TEXT("false"),
		static_cast<long long>(Value.IntegerValue), Value.NumberValue, *Value.StringValue,
		Value.BytesValue.Num(), *FString::Join(Value.ObjectKeys, TEXT(",")), Value.Elements.Num());
}

FString FirstRuntimeValueDifference(
	const FAssetDocumentRuntimeValueDto& Actual,
	const FAssetDocumentRuntimeValueDto& Expected,
	const FString& Path = TEXT("$"))
{
	auto Mismatch = [&](const TCHAR* Field)
	{
		return FString::Printf(TEXT("path=%s.%s actual={%s} expected={%s}"),
			*Path, Field, *RuntimeValueAuditSummary(Actual), *RuntimeValueAuditSummary(Expected));
	};
	if (Actual.Kind != Expected.Kind) return Mismatch(TEXT("Kind"));
	if (Actual.BooleanValue != Expected.BooleanValue) return Mismatch(TEXT("BooleanValue"));
	if (Actual.IntegerValue != Expected.IntegerValue) return Mismatch(TEXT("IntegerValue"));
	if (Actual.NumberValue != Expected.NumberValue) return Mismatch(TEXT("NumberValue"));
	if (Actual.StringValue != Expected.StringValue) return Mismatch(TEXT("StringValue"));
	if (Actual.BytesValue != Expected.BytesValue) return Mismatch(TEXT("BytesValue"));
	if (Actual.ObjectKeys != Expected.ObjectKeys) return Mismatch(TEXT("ObjectKeys"));
	if (Actual.Elements.Num() != Expected.Elements.Num()) return Mismatch(TEXT("Elements.Num"));
	for (int32 Index = 0; Index < Actual.Elements.Num(); ++Index)
	{
		const FString ChildPath = Actual.Kind == EAssetDocumentRuntimeValueKind::Object
			&& Actual.ObjectKeys.IsValidIndex(Index)
			? FString::Printf(TEXT("%s.%s"), *Path, *Actual.ObjectKeys[Index])
			: FString::Printf(TEXT("%s[%d]"), *Path, Index);
		const FString Difference = FirstRuntimeValueDifference(
			Actual.Elements[Index], Expected.Elements[Index], ChildPath);
		if (!Difference.IsEmpty()) return Difference;
	}
	return {};
}

const FAssetDocumentRuntimeValueDto* FindRuntimeValueField(
	const FAssetDocumentRuntimeValueDto& Object,
	const TCHAR* Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()) return nullptr;
	const int32 Index = Object.ObjectKeys.IndexOfByKey(Name);
	return Index == INDEX_NONE ? nullptr : &Object.Elements[Index];
}

FAssetDocumentContractValidationResult ValidResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult RejectedResult(const TCHAR* Code)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/BlackboardLifecycle");
	Diagnostic.Code = Code;
	Diagnostic.Message = TEXT("Test transaction deliberately rejected this enlistment.");
	return Result;
}

// This transaction is deliberately stricter than a counter: it retains the
// actual shared rollback actions, refuses a footprint without a matching prior
// action, records the public ordering, and executes registered coverage in
// reverse during rollback.  It is provider-local test infrastructure only.
class FBlackboardLifecycleTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FBlackboardLifecycleTransaction()
	{
		Token.RequestId = FGuid::NewGuid();
		Token.TransactionId = FGuid::NewGuid();
		Token.RuntimeRevision = 1;
	}
	explicit FBlackboardLifecycleTransaction(FAssetDocumentMutationToken InToken)
		: Token(MoveTemp(InToken))
	{
	}

	virtual FGuid GetTransactionId() const override { return Token.TransactionId; }
	virtual EAssetDocumentAtomicState GetState() const override { return State; }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		const int32 CallIndex = FootprintEnlistCount++;
		if (CallIndex == RejectFootprintAtCall)
		{
			Order.Add(TEXT("footprint-rejected"));
			return RejectedResult(TEXT("Test.RejectFootprint"));
		}
		for (const FAssetDocumentMutationFootprint& Item : Footprint)
		{
			const int32 ActionIndex = Actions.IndexOfByPredicate([&Item](const TSharedRef<IAssetDocumentRollbackAction>& Action)
			{
				return Action->GetRollbackComponentId() == Item.RollbackComponentId;
			});
			if (ActionIndex == INDEX_NONE)
			{
				Order.Add(TEXT("footprint-without-action"));
				return RejectedResult(TEXT("Test.MissingRollbackAction"));
			}
			RollbackTargets.AddUnique(ActionIndex);
		}
		Order.Add(TEXT("footprint"));
		EnlistedFootprint.Append(Footprint.GetData(), Footprint.Num());
		State = EAssetDocumentAtomicState::Mutating;
		if (RenameAfterFootprint != nullptr)
		{
			bRenameAfterFootprintSucceeded = RenameAfterFootprint->Rename(
				*RenameAfterFootprintTo, nullptr,
				REN_DontCreateRedirectors);
		}
		return ValidResult();
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		State = EAssetDocumentAtomicState::ReadyToCommit;
		return ValidResult();
	}
	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		State = EAssetDocumentAtomicState::Committed;
		Actions.Reset();
		RollbackTargets.Reset();
		return ValidResult();
	}
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override
	{
		FAssetDocumentContractValidationResult Result = ValidResult();
		for (int32 TargetIndex = RollbackTargets.Num() - 1; TargetIndex >= 0; --TargetIndex)
		{
			const TSharedRef<IAssetDocumentRollbackAction>& Action = Actions[RollbackTargets[TargetIndex]];
			Order.Add(FString::Printf(TEXT("rollback:%.*s"), Action->GetRollbackComponentId().Len(), Action->GetRollbackComponentId().GetData()));
			const FAssetDocumentContractValidationResult ActionResult = Action->Rollback(StableFailureCode);
			if (!ActionResult.bValid)
			{
				Result = ActionResult;
			}
		}
		State = Result.bValid ? EAssetDocumentAtomicState::RolledBack : EAssetDocumentAtomicState::Poisoned;
		Actions.Reset();
		RollbackTargets.Reset();
		return Result;
	}
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		const int32 CallIndex = ActionEnlistCount++;
		if (CallIndex == RejectActionAtCall)
		{
			Order.Add(TEXT("action-rejected"));
			return RejectedResult(TEXT("Test.RejectRollbackAction"));
		}
		Order.Add(TEXT("action"));
		Actions.Add(MoveTemp(Action));
		return ValidResult();
	}
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return ValidResult();
	}

	int32 RejectActionAtCall = INDEX_NONE;
	int32 RejectFootprintAtCall = INDEX_NONE;
	int32 ActionEnlistCount = 0;
	int32 FootprintEnlistCount = 0;
	UObject* RenameAfterFootprint = nullptr;
	FString RenameAfterFootprintTo;
	bool bRenameAfterFootprintSucceeded = false;
	TArray<FString> Order;
	TArray<FAssetDocumentMutationFootprint> EnlistedFootprint;

private:
	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	TArray<int32> RollbackTargets;
};

// The real Editor primitive alone creates the payload.  This test-local target
// slot intentionally models only a provider-local, already-completed handoff
// result; runtime handoff dispatch/persistence is owned by shared integration
// tests and is not claimed here.
class FBlackboardLifecycleHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
		FString ProviderId;
		uint32 Generation = 1;
		bool bInvalidated = false;
		TSharedPtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(FAssetDocumentOpaqueHandle InHandle, IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle)), Payload(&InPayload) {}
		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
		virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
		{
			check(Payload != nullptr);
			return *Payload;
		}
	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

	FBlackboardLifecycleHandleResolver() = default;
	explicit FBlackboardLifecycleHandleResolver(const FAssetDocumentMutationToken& InToken)
		: Token(InToken) {}
	void SetMutationToken(const FAssetDocumentMutationToken& InToken)
	{
		Token = InToken;
	}

	virtual FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access, TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive()) return Result;
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.ProviderId = TEXT("editor");
		Entry.Payload = MakeShareable(Payload.Release());
		Result.Handle = MakeHandle(Entry, Entries.Num());
		return Result;
	}
	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata = ValidateAssetDocumentHandleMetadata(Handle, Context);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index)) return Result;
		FEntry& Entry = Entries[Index];
		if (Entry.Generation == Handle.Generation && Entry.Kind == Handle.Kind
			&& Entry.Access == Handle.Access && Entry.ProviderId == Handle.ProviderId
			&& !Entry.bInvalidated && Entry.Payload.IsValid() && Entry.Payload->IsAlive())
		{
			Result.Lease = MakeUnique<FLease>(Handle, *Entry.Payload);
		}
		return Result;
	}
	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		FAssetDocumentHandleInvalidationResult Result;
		for (FEntry& Entry : Entries)
		{
			if (!Entry.bInvalidated && Entry.ProviderId == Event.ProviderId
				&& Event.RequestId == Token.RequestId && Event.TransactionId == Token.TransactionId
				&& Event.RuntimeRevision == Token.RuntimeRevision)
			{
				Entry.bInvalidated = true;
				++Result.InvalidatedHandleCount;
			}
		}
		return Result;
	}

	TOptional<FAssetDocumentOpaqueHandle> AddProviderLocalAIResult(const FAssetDocumentOpaqueHandle& EditorHandle)
	{
		const int32 Index = static_cast<int32>(EditorHandle.Slot) - 1;
		if (!Entries.IsValidIndex(Index)) return {};
		const FEntry& Source = Entries[Index];
		if (EditorHandle.RequestId != Token.RequestId || EditorHandle.TransactionId != Token.TransactionId
			|| EditorHandle.RuntimeRevision != Token.RuntimeRevision
			|| EditorHandle.Kind != EAssetDocumentHandleKind::Asset
			|| EditorHandle.Access != EAssetDocumentHandleAccess::Mutable
			|| Source.ProviderId != TEXT("editor") || Source.Generation != EditorHandle.Generation
			|| !Source.Payload.IsValid() || !Source.Payload->IsAlive()) return {};
		FEntry& Target = Entries.AddDefaulted_GetRef();
		Target.Kind = Source.Kind;
		Target.Access = Source.Access;
		Target.ProviderId = TEXT("ai");
		Target.Payload = Source.Payload;
		return MakeHandle(Target, Entries.Num());
	}

private:
	FAssetDocumentOpaqueHandle MakeHandle(const FEntry& Entry, const int32 OneBasedSlot) const
	{
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Token.RequestId;
		Handle.TransactionId = Token.TransactionId;
		Handle.RuntimeRevision = Token.RuntimeRevision;
		Handle.ProviderId = Entry.ProviderId;
		Handle.Slot = static_cast<uint32>(OneBasedSlot);
		Handle.Generation = Entry.Generation;
		Handle.Kind = Entry.Kind;
		Handle.Access = Entry.Access;
		return Handle;
	}

	FAssetDocumentMutationToken Token;
	TArray<FEntry> Entries;
};

FAssetDocumentPrimitiveInvocation MakeResolveExistingInvocation(
	const UObject& Object, const FAssetDocumentMutationToken& Token)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = TEXT("editor.asset.resolve_existing.v1");
	Invocation.Version = 1;
	Invocation.RequestId = Token.RequestId;
	Invocation.TransactionId = Token.TransactionId;
	Invocation.RuntimeRevision = Token.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.ValueArguments = ObjectValue({TEXT("expected_class"), TEXT("path")},
		{StringValue(Object.GetClass()->GetPathName()), StringValue(Object.GetPathName())});
	return Invocation;
}

TOptional<FAssetDocumentOpaqueHandle> ResolveEditorMutableAsset(
	FAutomationTestBase& Test, const UObject& Object,
	const FAssetDocumentMutationToken& Token, FBlackboardLifecycleHandleResolver& Handles)
{
	const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
		MakeResolveExistingInvocation(Object, Token), Handles, nullptr);
	if (!Test.TestEqual(TEXT("real editor.asset.resolve_existing.v1 succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return {};
	for (const FAssetDocumentPrimitiveHandleResult& Result : Outcome.HandleResults)
	{
		if (Result.Name == TEXT("asset_mutable") && Result.Handles.Num() == 1) return Result.Handles[0];
	}
	Test.AddError(TEXT("editor.asset.resolve_existing.v1 did not issue exactly one mutable Asset handle"));
	return {};
}

class FStructuralPublicationNoCancellation final
	: public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override
	{
		return false;
	}
};

struct FStructuralPublicationRealAtomicScenario
{
	FBlackboardLifecycleHandleResolver Handles;
	TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction;
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease;
	FAssetDocumentMutationFootprint Footprint;

	bool Begin(FAutomationTestBase& Test, UBlackboardData& Blackboard)
	{
		Footprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
		Footprint.StableId = TEXT("asset:StructuralPublication/Keys[*]");
		Footprint.RollbackComponentId =
			TEXT("assetdocument.ai.blackboard.local_keys.v1");
		FAssetDocumentPreflightReceipt Receipt;
		Receipt.RequestId = FGuid::NewGuid();
		Receipt.RuntimeRevision = 1;
		Receipt.CompleteFootprint = {Footprint};
		Receipt.ReceiptHash = TEXT("sha256:structural-publication-real-atomic");
		Receipt.bSucceeded = true;
		TArray<TSharedRef<IAssetDocumentRollbackAction>> InitialActions;
		Composition = MakeUnique<FAssetDocumentRollbackActionComposition>(
			Handles, MoveTemp(InitialActions),
			[](EAssetDocumentRuntimeRequestPhase,
				const FAssetDocumentRuntimeDiagnostic&) {});
		if (!Test.TestTrue(TEXT("real rollback-action composition is valid"),
			Composition->IsValid()))
		{
			return false;
		}
		FStructuralPublicationNoCancellation Cancellation;
		Transaction = Composition->BeginLiveAfterPreflight(Receipt, Cancellation);
		if (!Test.TestNotNull(TEXT("real Atomic runtime begins after preflight"),
			Transaction.Get()))
		{
			return false;
		}
		Handles.SetMutationToken(Transaction->GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Editor =
			ResolveEditorMutableAsset(
				Test, Blackboard, Transaction->GetMutationToken(), Handles);
		if (!Editor.IsSet())
		{
			return false;
		}
		const TOptional<FAssetDocumentOpaqueHandle> AI =
			Handles.AddProviderLocalAIResult(Editor.GetValue());
		if (!Test.TestTrue(TEXT("real Atomic fixture obtains AI mutable Asset handle"),
			AI.IsSet()))
		{
			return false;
		}
		const FAssetDocumentMutationToken& Token = Transaction->GetMutationToken();
		FAssetDocumentHandleValidationContext Context{
			Token.RequestId,
			Token.TransactionId,
			Token.RuntimeRevision,
			TEXT("ai"),
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable};
		FAssetDocumentVerifiedLeaseResult Acquired =
			Handles.AcquireVerifiedLease(AI.GetValue(), Context);
		Lease = MoveTemp(Acquired.Lease);
		return Test.TestNotNull(TEXT("real Atomic fixture obtains verified AI lease"),
			Lease.Get());
	}
};

int32 CountDirectBlackboardKeyTypes(const UBlackboardData& Blackboard)
{
	TArray<UObject*> DirectChildren;
	GetObjectsWithOuter(&Blackboard, DirectChildren, AssetDocumentObjects::DirectOnly);
	int32 Count = 0;
	for (const UObject* Object : DirectChildren)
	{
		Count += Object != nullptr && Object->IsA<UBlackboardKeyType>() ? 1 : 0;
	}
	return Count;
}

TSharedPtr<IPropertyHandle> MakeExactKeysHandle(
	FAutomationTestBase& Test,
	UBlackboardData& Blackboard,
	TSharedPtr<IPropertyRowGenerator>& OutRows)
{
	const TSet<FString> KeysAllowList = {
		GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString()};
	if (!Test.TestTrue(TEXT("exact Keys rows are available"),
		AssetDocumentAIBlackboardMutation::TryMakeRows(
			Blackboard, OutRows, &KeysAllowList)))
	{
		return {};
	}
	const TSharedPtr<IPropertyHandle> KeysHandle =
		AssetDocumentAIBlackboardMutation::FindUniqueOwnedPropertyHandle(
			*OutRows, Blackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
	Test.TestTrue(TEXT("exact Keys handle is available"), KeysHandle.IsValid());
	return KeysHandle;
}

FAssetDocumentDurableCompletionReceipt MakeStructuralCompletionReceipt(
	const IAssetDocumentAtomicTransaction& Transaction)
{
	FAssetDocumentDurableCompletionReceipt Receipt;
	Receipt.RequestId = Transaction.GetMutationToken().RequestId;
	Receipt.TransactionId = Transaction.GetTransactionId();
	Receipt.JournalId = TEXT("structural-publication-journal");
	Receipt.PackageContentDigest = TEXT("sha256:structural-publication-package");
	Receipt.SidecarContentDigest = TEXT("sha256:structural-publication-sidecar");
	Receipt.bSyncStateAdvanced = true;
	return Receipt;
}

TOptional<FAssetDocumentOpaqueHandle> ResolveAIMutableAsset(
	FAutomationTestBase& Test,
	const UObject& Object,
	FBlackboardLifecycleTransaction& Transaction,
	FBlackboardLifecycleHandleResolver& Handles)
{
	const TOptional<FAssetDocumentOpaqueHandle> Editor = ResolveEditorMutableAsset(
		Test, Object, Transaction.GetMutationToken(), Handles);
	if (!Editor.IsSet()) return {};
	const TOptional<FAssetDocumentOpaqueHandle> AI =
		Handles.AddProviderLocalAIResult(Editor.GetValue());
	Test.TestTrue(TEXT("provider-local AI mutable Asset fixture exists"), AI.IsSet());
	return AI;
}

FAssetDocumentPrimitiveInvocation MakeInvocation(const TCHAR* PrimitiveId,
	const FAssetDocumentOpaqueHandle& AssetHandle, const UBlackboardData& Blackboard,
	EAssetDocumentRuntimeRequestPhase Phase, const FString& ParentPath = FString(),
	const TArray<FAssetDocumentRuntimeValueDto>& OrderedKeys = {})
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = PrimitiveId;
	Invocation.Version = 1;
	Invocation.RequestId = AssetHandle.RequestId;
	Invocation.TransactionId = AssetHandle.TransactionId;
	Invocation.RuntimeRevision = AssetHandle.RuntimeRevision;
	Invocation.Phase = Phase;
	if (Phase == EAssetDocumentRuntimeRequestPhase::Apply)
	{
		Invocation.HandleArguments = {{TEXT("asset"), AssetHandle}};
	}
	const FString BlackboardPath = Blackboard.GetPathName();
	const FString KeyClassPath = UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass()->GetPathName();
	if (FCString::Strcmp(PrimitiveId, KeyTypeInspectId) == 0)
	{
		Invocation.ValueArguments = ObjectValue({TEXT("blackboard_path"), TEXT("expected_class_path"), TEXT("key_name")},
			{StringValue(BlackboardPath), StringValue(KeyClassPath), StringValue(TEXT("ExistingKey"))});
	}
	else if (FCString::Strcmp(PrimitiveId, KeyTypeApplyId) == 0)
	{
		Invocation.ValueArguments = ObjectValue(
			{TEXT("blackboard_path"), TEXT("canonical_properties"), TEXT("expected_class_path"), TEXT("key_name")},
			{StringValue(BlackboardPath), ObjectValue({TEXT("Int32Value")}, {IntegerValue(41)}),
				StringValue(KeyClassPath), StringValue(TEXT("ExistingKey"))});
	}
	else if (FCString::Strcmp(PrimitiveId, ParentSetId) == 0)
	{
		Invocation.ValueArguments = ObjectValue({TEXT("blackboard_path"), TEXT("parent_path_or_null")},
			{StringValue(BlackboardPath), ParentPath.IsEmpty() ? NullValue() : StringValue(ParentPath)});
	}
	else
	{
		Invocation.ValueArguments = ObjectValue({TEXT("blackboard_path"), TEXT("ordered_keys")},
			{StringValue(BlackboardPath), ArrayValue(OrderedKeys)});
	}
	return Invocation;
}

FAssetDocumentPrimitiveInvocation MakeEntryReadbackInvocation(
	const UObject& Object,
	const FAssetDocumentMutationToken& Token,
	const FString& KeyName,
	const EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Verify)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = EntryReadbackV2Id;
	Invocation.Version = 2;
	Invocation.RequestId = Token.RequestId;
	Invocation.TransactionId = Token.TransactionId;
	Invocation.RuntimeRevision = Token.RuntimeRevision;
	Invocation.Phase = Phase;
	Invocation.ValueArguments = ObjectValue(
		{TEXT("blackboard_path"), TEXT("key_name")},
		{StringValue(Object.GetPathName()), StringValue(KeyName)});
	return Invocation;
}

FAssetDocumentPrimitiveInvocation MakeParentReadbackInvocation(
	const UObject& Object,
	const FAssetDocumentMutationToken& Token,
	const EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Verify)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = ParentReadbackV2Id;
	Invocation.Version = 2;
	Invocation.RequestId = Token.RequestId;
	Invocation.TransactionId = Token.TransactionId;
	Invocation.RuntimeRevision = Token.RuntimeRevision;
	Invocation.Phase = Phase;
	Invocation.ValueArguments = ObjectValue({TEXT("blackboard_path")}, {StringValue(Object.GetPathName())});
	return Invocation;
}

FAssetDocumentPrimitiveInvocation MakeParentSetV2Invocation(
	const FAssetDocumentOpaqueHandle& AssetHandle,
	const UObject& Blackboard,
	const UObject* Parent)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = ParentSetV2Id;
	Invocation.Version = 2;
	Invocation.RequestId = AssetHandle.RequestId;
	Invocation.TransactionId = AssetHandle.TransactionId;
	Invocation.RuntimeRevision = AssetHandle.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.HandleArguments = {{TEXT("asset"), AssetHandle}};
	Invocation.ValueArguments = ObjectValue(
		{TEXT("blackboard_path"), TEXT("parent_path_or_null")},
		{StringValue(Blackboard.GetPathName()),
			Parent == nullptr ? NullValue() : StringValue(Parent->GetPathName())});
	return Invocation;
}

FAssetDocumentPrimitiveInvocation MakeLocalKeysReadbackInvocation(
	const UObject& Object,
	const FAssetDocumentMutationToken& Token,
	const EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Verify)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = LocalKeysReadbackV2Id;
	Invocation.Version = 2;
	Invocation.RequestId = Token.RequestId;
	Invocation.TransactionId = Token.TransactionId;
	Invocation.RuntimeRevision = Token.RuntimeRevision;
	Invocation.Phase = Phase;
	Invocation.ValueArguments = ObjectValue({TEXT("blackboard_path")}, {StringValue(Object.GetPathName())});
	return Invocation;
}

FAssetDocumentPrimitiveInvocation MakeUpdateExistingPrerequisiteInvocation(
	const FAssetDocumentOpaqueHandle& AssetHandle)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = TEXT("ai.blackboard.entry.update_existing.v2");
	Invocation.Version = 2;
	Invocation.RequestId = AssetHandle.RequestId;
	Invocation.TransactionId = AssetHandle.TransactionId;
	Invocation.RuntimeRevision = AssetHandle.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.HandleArguments = {{TEXT("asset"), AssetHandle}};
	return Invocation;
}

FAssetDocumentPrimitiveInvocation MakeUpdateExistingInvocation(
	const FAssetDocumentOpaqueHandle& AssetHandle,
	const AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeUpdateExistingPrerequisiteInvocation(AssetHandle);
	Invocation.ValueArguments = ObjectValue(
		{TEXT("blackboard_path"), TEXT("entry"), TEXT("key_name")},
		{StringValue(Request.BlackboardPath),
			ObjectValue(
				{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
					TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
				{StringValue(Request.Category), StringValue(Request.Description),
					ObjectValue({TEXT("Kind"), TEXT("Path")},
						{StringValue(TEXT("ClassRef")), StringValue(Request.ExpectedClassPath)}),
					Request.CanonicalProperties, StringValue(Request.KeyName),
					BooleanValue(Request.bInstanceSynced)}),
			StringValue(Request.KeyName)});
	return Invocation;
}

FAssetDocumentRuntimeValueDto MakeEntryAddV2Entry(
	const FString& Name,
	const int32 Int32Value = 41,
	const FString& StringValueText = TEXT("Added property"))
{
	return ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("AddedCategory")), StringValue(TEXT("Added description")),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("ClassRef")),
					StringValue(UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("Int32Value"), TEXT("StringValue")},
				{IntegerValue(Int32Value), StringValue(StringValueText)}),
			StringValue(Name), BooleanValue(true)});
}

FAssetDocumentRuntimeValueDto MakeEntryAddV2IntEntry(
	const FString& Name,
	const int32 DefaultValue)
{
	return ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("Persistence")), StringValue(TEXT("Survives fresh reload")),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("ClassRef")),
					StringValue(UBlackboardKeyType_Int::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("DefaultValue")}, {IntegerValue(DefaultValue)}),
			StringValue(Name), BooleanValue(true)});
}

FAssetDocumentPrimitiveInvocation MakeEntryAddV2Invocation(
	const FAssetDocumentOpaqueHandle& AssetHandle,
	const UObject& Blackboard,
	FAssetDocumentRuntimeValueDto Entry,
	const int64 Index)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = EntryAddV2Id;
	Invocation.Version = 2;
	Invocation.RequestId = AssetHandle.RequestId;
	Invocation.TransactionId = AssetHandle.TransactionId;
	Invocation.RuntimeRevision = AssetHandle.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.HandleArguments = {{TEXT("asset"), AssetHandle}};
	Invocation.ValueArguments = ObjectValue(
		{TEXT("blackboard_path"), TEXT("entry"), TEXT("insertion")},
		{StringValue(Blackboard.GetPathName()), MoveTemp(Entry),
			ObjectValue({TEXT("Index"), TEXT("Kind")},
				{IntegerValue(Index), StringValue(TEXT("AtIndex"))})});
	return Invocation;
}

FAssetDocumentRuntimeValueDto MakeEntryReplaceClassV2IntEntry(
	const FString& Name,
	const int32 DefaultValue = 73)
{
	return ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("Replacement")), StringValue(TEXT("Replaced class")),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("ClassRef")),
					StringValue(UBlackboardKeyType_Int::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("DefaultValue")}, {IntegerValue(DefaultValue)}),
			StringValue(Name), BooleanValue(true)});
}

FAssetDocumentRuntimeValueDto MakeEntryReplaceClassV2OriginalEntry(
	const FString& Name,
	const int32 Int32Value = 7,
	const FString& Text = FString())
{
	return ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(FString()), StringValue(FString()),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("ClassRef")),
					StringValue(UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("Int32Value"), TEXT("StringValue")},
				{IntegerValue(Int32Value), StringValue(Text)}),
			StringValue(Name), BooleanValue(false)});
}

FAssetDocumentPrimitiveInvocation MakeEntryReplaceClassV2Invocation(
	const FAssetDocumentOpaqueHandle& AssetHandle,
	const UObject& Blackboard,
	const FString& KeyName,
	FAssetDocumentRuntimeValueDto Entry)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = EntryReplaceClassV2Id;
	Invocation.Version = 2;
	Invocation.RequestId = AssetHandle.RequestId;
	Invocation.TransactionId = AssetHandle.TransactionId;
	Invocation.RuntimeRevision = AssetHandle.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.HandleArguments = {{TEXT("asset"), AssetHandle}};
	Invocation.ValueArguments = ObjectValue(
		{TEXT("blackboard_path"), TEXT("entry"), TEXT("key_name")},
		{StringValue(Blackboard.GetPathName()), MoveTemp(Entry), StringValue(KeyName)});
	return Invocation;
}

FAssetDocumentPrimitiveInvocation MakeEntryRemoveV2Invocation(
	const FAssetDocumentOpaqueHandle& AssetHandle,
	const UObject& Blackboard,
	const FString& KeyName)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = EntryRemoveV2Id;
	Invocation.Version = 2;
	Invocation.RequestId = AssetHandle.RequestId;
	Invocation.TransactionId = AssetHandle.TransactionId;
	Invocation.RuntimeRevision = AssetHandle.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.HandleArguments = {{TEXT("asset"), AssetHandle}};
	Invocation.ValueArguments = ObjectValue(
		{TEXT("blackboard_path"), TEXT("key_name")},
		{StringValue(Blackboard.GetPathName()), StringValue(KeyName)});
	return Invocation;
}

FAssetDocumentPrimitiveInvocation MakeLocalKeysReorderV2Invocation(
	const FAssetDocumentOpaqueHandle& AssetHandle,
	const UObject& Blackboard,
	TArray<FAssetDocumentRuntimeValueDto> OrderedLocalKeyNames)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = LocalKeysReorderV2Id;
	Invocation.Version = 2;
	Invocation.RequestId = AssetHandle.RequestId;
	Invocation.TransactionId = AssetHandle.TransactionId;
	Invocation.RuntimeRevision = AssetHandle.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.HandleArguments = {{TEXT("asset"), AssetHandle}};
	Invocation.ValueArguments = ObjectValue(
		{TEXT("blackboard_path"), TEXT("ordered_local_key_names")},
		{StringValue(Blackboard.GetPathName()), ArrayValue(MoveTemp(OrderedLocalKeyNames))});
	return Invocation;
}

FAssetDocumentRuntimeValueDto ExistingKeyRow(const TCHAR* Name, const int64 Int32Value = 7)
{
	return ObjectValue(
		{TEXT("canonical_properties"), TEXT("key_type_class_path"), TEXT("name")},
		{ObjectValue({TEXT("Int32Value")}, {IntegerValue(Int32Value)}),
			StringValue(UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass()->GetPathName()), StringValue(Name)});
}

struct FBlackboardObservation
{
	UBlackboardData* Parent = nullptr;
	TArray<FName> Names;
	TArray<UBlackboardKeyType*> Instances;
	TArray<int32> IntValues;
	TArray<FName> DerivedNames;
	TArray<UClass*> DerivedTypes;
	TArray<bool> DerivedInstanceSynced;
	FBlackboard::FKey FirstKeyId = FBlackboard::InvalidKey;
	bool bHasSynchronizedKeys = false;
	bool bDirty = false;

	explicit FBlackboardObservation(UBlackboardData& Blackboard)
		: Parent(Blackboard.Parent)
		, FirstKeyId(Blackboard.GetFirstKeyID())
		, bHasSynchronizedKeys(Blackboard.HasSynchronizedKeys())
		, bDirty(Blackboard.GetOutermost()->IsDirty())
	{
		for (const FBlackboardEntry& Entry : Blackboard.Keys)
		{
			Names.Add(Entry.EntryName);
			Instances.Add(Entry.KeyType);
			const UAssetDocumentAITypedPreimageTestLocalKeyType* Key =
				Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Entry.KeyType);
			IntValues.Add(Key != nullptr ? Key->Int32Value : INDEX_NONE);
		}
		for (int32 Index = 0; Index < Blackboard.GetNumKeys(); ++Index)
		{
			const FBlackboard::FKey KeyId(Index);
			DerivedNames.Add(Blackboard.GetKeyName(KeyId));
			DerivedTypes.Add(Blackboard.GetKeyType(KeyId).Get());
			DerivedInstanceSynced.Add(Blackboard.IsKeyInstanceSynced(KeyId));
		}
	}

	bool Equals(const UBlackboardData& Blackboard) const
	{
		if (Parent != Blackboard.Parent || bDirty != Blackboard.GetOutermost()->IsDirty()
			|| Names.Num() != Blackboard.Keys.Num()
			|| FirstKeyId != Blackboard.GetFirstKeyID()
			|| bHasSynchronizedKeys != Blackboard.HasSynchronizedKeys()
			|| DerivedNames.Num() != Blackboard.GetNumKeys()) return false;
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			const FBlackboardEntry& Entry = Blackboard.Keys[Index];
			const UAssetDocumentAITypedPreimageTestLocalKeyType* Key =
				Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Entry.KeyType);
			if (Entry.EntryName != Names[Index] || Entry.KeyType != Instances[Index]
				|| (Key != nullptr ? Key->Int32Value : INDEX_NONE) != IntValues[Index]) return false;
		}
		for (int32 Index = 0; Index < DerivedNames.Num(); ++Index)
		{
			const FBlackboard::FKey KeyId(Index);
			if (Blackboard.GetKeyName(KeyId) != DerivedNames[Index]
				|| Blackboard.GetKeyType(KeyId).Get() != DerivedTypes[Index]
				|| Blackboard.IsKeyInstanceSynced(KeyId) != DerivedInstanceSynced[Index]) return false;
		}
		return true;
	}
};

bool RequireObservation(
	FAutomationTestBase& Test,
	const FString& Label,
	const FBlackboardObservation& Before,
	const UBlackboardData& Blackboard)
{
	if (Before.Equals(Blackboard))
	{
		return true;
	}
	TArray<FName> ActualNames;
	TArray<int32> ActualValues;
	for (const FBlackboardEntry& Entry : Blackboard.Keys)
	{
		ActualNames.Add(Entry.EntryName);
		const UAssetDocumentAITypedPreimageTestLocalKeyType* Key =
			Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Entry.KeyType);
		ActualValues.Add(Key != nullptr ? Key->Int32Value : INDEX_NONE);
	}
	TArray<FName> ActualDerivedNames;
	for (int32 Index = 0; Index < Blackboard.GetNumKeys(); ++Index)
	{
		ActualDerivedNames.Add(Blackboard.GetKeyName(FBlackboard::FKey(Index)));
	}
	Test.AddError(FString::Printf(
		TEXT("%s mismatch: parent %s -> %s, dirty %d -> %d, names %s -> %s, values %s -> %s, derived %s -> %s, first-id %d -> %d, synchronized %d -> %d"),
		*Label,
		*GetPathNameSafe(Before.Parent), *GetPathNameSafe(Blackboard.Parent),
		Before.bDirty, Blackboard.GetOutermost()->IsDirty(),
		*FString::JoinBy(Before.Names, TEXT("|"), [](FName Name) { return Name.ToString(); }),
		*FString::JoinBy(ActualNames, TEXT("|"), [](FName Name) { return Name.ToString(); }),
		*FString::JoinBy(Before.IntValues, TEXT("|"), [](int32 Value) { return FString::FromInt(Value); }),
		*FString::JoinBy(ActualValues, TEXT("|"), [](int32 Value) { return FString::FromInt(Value); }),
		*FString::JoinBy(Before.DerivedNames, TEXT("|"), [](FName Name) { return Name.ToString(); }),
		*FString::JoinBy(ActualDerivedNames, TEXT("|"), [](FName Name) { return Name.ToString(); }),
		static_cast<int32>(Before.FirstKeyId), static_cast<int32>(Blackboard.GetFirstKeyID()),
		Before.bHasSynchronizedKeys, Blackboard.HasSynchronizedKeys()));
	return false;
}

void AddExistingKey(UBlackboardData& Blackboard, const FName Name, const int32 Value)
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	UAssetDocumentAITypedPreimageTestLocalKeyType* Key =
		NewObject<UAssetDocumentAITypedPreimageTestLocalKeyType>(&Blackboard);
	Key->Int32Value = Value;
	Entry.KeyType = Key;
	Blackboard.Keys.Add(Entry);
}

void AddExistingStockIntKey(
	UBlackboardData& Blackboard, const FName Name, const int32 DefaultValue)
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	UBlackboardKeyType_Int* Key = NewObject<UBlackboardKeyType_Int>(&Blackboard);
	Key->DefaultValue = DefaultValue;
	Entry.KeyType = Key;
	Blackboard.Keys.Add(Entry);
}

bool IsEngineDerivedSelfKey(const FBlackboardEntry& Entry)
{
	const UBlackboardKeyType_Object* ObjectKey =
		Cast<UBlackboardKeyType_Object>(Entry.KeyType);
	return Entry.EntryName == FBlackboard::KeySelf
		&& ObjectKey != nullptr
		&& ObjectKey->GetClass() == UBlackboardKeyType_Object::StaticClass()
		&& ObjectKey->BaseClass == AActor::StaticClass()
		&& ObjectKey->DefaultValue == nullptr
		&& Entry.EntryDescription.IsEmpty()
		&& Entry.EntryCategory.IsNone()
		&& !Entry.bInstanceSynced;
}

int32 CountAuthoredLocalKeys(const UBlackboardData& Blackboard)
{
	int32 Count = 0;
	for (const FBlackboardEntry& Entry : Blackboard.Keys)
	{
		Count += !IsEngineDerivedSelfKey(Entry);
	}
	return Count;
}

void SetFixtureParent(
	UBlackboardData& Blackboard,
	UBlackboardData* Parent,
	UBlackboardData& Child)
{
	Blackboard.Parent = Parent;
	Blackboard.UpdateParentKeys();
	Blackboard.UpdateKeyIDs();
	Blackboard.UpdateIfHasSynchronizedKeys();
	Child.UpdateParentKeys();
	Child.UpdateKeyIDs();
	Child.UpdateIfHasSynchronizedKeys();
}

bool RequireSuccess(FAutomationTestBase& Test, const FString& Label,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Outcome.Diagnostics)
		{
			Test.AddError(FString::Printf(TEXT("%s diagnostic %s: %s"),
				*Label, *Diagnostic.Code, *Diagnostic.Message));
		}
	}
	return Test.TestEqual(*(Label + TEXT(" succeeds")), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		&& Test.TestTrue(*(Label + TEXT(" has no diagnostics")), Outcome.Diagnostics.IsEmpty());
}

bool RequireNoForwardWrite(FAutomationTestBase& Test, const FString& Label,
	const FAssetDocumentPrimitiveOutcome& Outcome, const FBlackboardObservation& Before,
	const UBlackboardData& Blackboard, const FBlackboardLifecycleTransaction& Transaction)
{
	return Test.TestEqual(*(Label + TEXT(" fails")), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed)
		&& Test.TestTrue(*(Label + TEXT(" clears output value/handles/ChangedSet")),
			Outcome.Value.Kind == EAssetDocumentRuntimeValueKind::Object
			&& Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty()
			&& Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty())
		&& Test.TestTrue(*(Label + TEXT(" preserves authored state and dirty bit")), Before.Equals(Blackboard))
		&& Test.TestTrue(*(Label + TEXT(" has no completed footprint")), Transaction.EnlistedFootprint.IsEmpty());
}

bool RequireReadbackFailure(
	FAutomationTestBase& Test,
	const FString& Label,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FBlackboardObservation& Before,
	const UBlackboardData& Blackboard,
	const FBlackboardLifecycleTransaction& Transaction,
	const TCHAR* ExpectedDiagnostic = nullptr)
{
	const bool bNoFootprint = RequireNoForwardWrite(
		Test, Label, Outcome, Before, Blackboard, Transaction);
	const bool bHasDiagnostic = Test.TestFalse(
		*(Label + TEXT(" has a closed or preserved-runtime diagnostic")),
		Outcome.Diagnostics.IsEmpty());
	if (ExpectedDiagnostic == nullptr)
	{
		return bNoFootprint && bHasDiagnostic;
	}
	return bNoFootprint && bHasDiagnostic
		&& Test.TestTrue(*(Label + TEXT(" has its exact readback diagnostic")),
			HasDiagnostic(Outcome, ExpectedDiagnostic));
}

bool RequireExactFailureTerminal(
	FAutomationTestBase& Test,
	const FString& Label,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FBlackboardObservation& Before,
	const UBlackboardData& Blackboard,
	const FBlackboardLifecycleTransaction& Transaction,
	const TCHAR* ExpectedDiagnostic)
{
	bool Passed = RequireNoForwardWrite(
		Test, Label, Outcome, Before, Blackboard, Transaction);
	Passed &= Test.TestEqual(*(Label + TEXT(" has exactly one diagnostic")),
		Outcome.Diagnostics.Num(), 1);
	if (Outcome.Diagnostics.Num() == 1)
	{
		Passed &= Test.TestEqual(*(Label + TEXT(" has its sole frozen terminal")),
			Outcome.Diagnostics[0].Code, FString(ExpectedDiagnostic));
		Passed &= Test.TestFalse(*(Label + TEXT(" leaks no Runtime validator diagnostic")),
			Outcome.Diagnostics[0].Code.StartsWith(
				TEXT("PrimitiveInvocation."), ESearchCase::CaseSensitive));
	}
	Passed &= Test.TestEqual(*(Label + TEXT(" observes no handler action")),
		Transaction.ActionEnlistCount, 0);
	Passed &= Test.TestEqual(*(Label + TEXT(" observes no footprint attempt")),
		Transaction.FootprintEnlistCount, 0);
	return Passed;
}

bool RequireCanonicalReadbackEntry(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const UBlackboardData& Blackboard,
	const FString& Name)
{
	if (!RequireSuccess(Test, TEXT("readback owned-local entry readback"), Outcome)
		|| !Test.TestEqual(TEXT("readback has only entry"), Outcome.Value.ObjectKeys,
			TArray<FString>{TEXT("entry")})
		|| !Test.TestTrue(TEXT("readback has one entry value"), Outcome.Value.Elements.Num() == 1))
	{
		return false;
	}

	const FAssetDocumentRuntimeValueDto& Entry = Outcome.Value.Elements[0];
	if (!Test.TestEqual(TEXT("readback uses the complete six-field entry shape"), Entry.ObjectKeys,
		TArray<FString>{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")})
		|| !Test.TestTrue(TEXT("readback has exactly six entry values"), Entry.Elements.Num() == 6))
	{
		return false;
	}

	const FAssetDocumentRuntimeValueDto& ClassRef = Entry.Elements[2];
	return Test.TestEqual(TEXT("readback preserves empty Category"), Entry.Elements[0].StringValue, FString())
		&& Test.TestEqual(TEXT("readback preserves empty Description"), Entry.Elements[1].StringValue, FString())
		&& Test.TestEqual(TEXT("readback canonicalizes the exact KeyType class reference"),
			ClassRef.ObjectKeys, TArray<FString>{TEXT("Kind"), TEXT("Path")})
		&& Test.TestTrue(TEXT("readback ClassRef has two values"), ClassRef.Elements.Num() == 2)
		&& Test.TestEqual(TEXT("readback ClassRef kind is exact"),
			ClassRef.Elements.IsValidIndex(0) ? ClassRef.Elements[0].StringValue : FString(), FString(TEXT("ClassRef")))
		&& Test.TestEqual(TEXT("readback ClassRef path is the exact loaded key type"),
			ClassRef.Elements.IsValidIndex(1) ? ClassRef.Elements[1].StringValue : FString(),
			UAssetDocumentAITypedPreimageTestZeroFieldKeyType::StaticClass()->GetPathName())
		&& Test.TestTrue(TEXT("readback preserves empty KeyTypeProperties"),
			Entry.Elements[3].Kind == EAssetDocumentRuntimeValueKind::Object
			&& Entry.Elements[3].ObjectKeys.IsEmpty() && Entry.Elements[3].Elements.IsEmpty())
		&& Test.TestEqual(TEXT("readback preserves the owned local name"), Entry.Elements[4].StringValue, Name)
		&& Test.TestFalse(TEXT("readback preserves false bInstanceSynced"), Entry.Elements[5].BooleanValue)
		&& Test.TestTrue(TEXT("readback has no handles or changed set"),
			Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty())
		&& Test.TestEqual(TEXT("readback object remains the exact Blackboard"),
			Blackboard.GetClass(), UBlackboardData::StaticClass());
}

bool RequireCanonicalParentReadback(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const UBlackboardData* ExpectedParent)
{
	if (!RequireSuccess(Test, TEXT("Parent readback"), Outcome)
		|| !Test.TestEqual(TEXT("Parent readback has exactly its canonical member"),
			Outcome.Value.ObjectKeys, TArray<FString>{TEXT("parent_path_or_null")})
		|| !Test.TestTrue(TEXT("Parent readback has exactly one value"), Outcome.Value.Elements.Num() == 1))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& ParentValue = Outcome.Value.Elements[0];
	if (ExpectedParent == nullptr)
	{
		return Test.TestEqual(TEXT("Parent readback preserves a null Parent"), ParentValue.Kind,
			EAssetDocumentRuntimeValueKind::Null);
	}
	return Test.TestEqual(TEXT("Parent readback returns canonical exact Parent path"),
		ParentValue.StringValue, ExpectedParent->GetPathName())
		&& Test.TestEqual(TEXT("Parent readback value is a string"), ParentValue.Kind,
			EAssetDocumentRuntimeValueKind::String)
		&& Test.TestTrue(TEXT("Parent readback creates no handles or ChangedSet"),
			Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
}

bool RequireCanonicalLocalKeysReadback(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TArray<FString>& ExpectedNames)
{
	if (!RequireSuccess(Test, TEXT("local-keys readback"), Outcome)
		|| !Test.TestEqual(TEXT("local-keys readback has exactly its canonical member"),
			Outcome.Value.ObjectKeys, TArray<FString>{TEXT("ordered_local_key_names")})
		|| !Test.TestTrue(TEXT("local-keys readback has exactly one value"), Outcome.Value.Elements.Num() == 1))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& Names = Outcome.Value.Elements[0];
	if (!Test.TestEqual(TEXT("local-keys readback returns a names array"), Names.Kind,
		EAssetDocumentRuntimeValueKind::Array) || !Test.TestEqual(TEXT("local-keys readback preserves count"),
		Names.Elements.Num(), ExpectedNames.Num())) return false;
	for (int32 Index = 0; Index < ExpectedNames.Num(); ++Index)
	{
		if (!Test.TestEqual(*FString::Printf(TEXT("local-keys readback preserves stored order at %d"), Index),
			Names.Elements[Index].StringValue, ExpectedNames[Index])
			|| !Test.TestEqual(*FString::Printf(TEXT("local-keys readback returns strings at %d"), Index),
			Names.Elements[Index].Kind, EAssetDocumentRuntimeValueKind::String)) return false;
	}
	return Test.TestTrue(TEXT("local-keys readback creates no handles or ChangedSet"),
		Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
}

bool RequireCanonicalPropertyReadback(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	if (!RequireSuccess(Test, TEXT("readback property canonicalization readback"), Outcome)
		|| !Test.TestTrue(TEXT("readback property canonicalization has one entry"),
			Outcome.Value.ObjectKeys == TArray<FString>{TEXT("entry")}
			&& Outcome.Value.Elements.Num() == 1))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& Entry = Outcome.Value.Elements[0];
	if (!Test.TestTrue(TEXT("readback property canonicalization has six entry fields"),
		Entry.ObjectKeys == TArray<FString>{TEXT("Category"), TEXT("Description"),
			TEXT("KeyTypeClass"), TEXT("KeyTypeProperties"), TEXT("Name"),
			TEXT("bInstanceSynced")} && Entry.Elements.Num() == 6))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& Properties = Entry.Elements[3];
	return Test.TestEqual(TEXT("readback property canonicalization preserves Category"),
		Entry.Elements[0].StringValue, FString(TEXT("Readback")))
		&& Test.TestEqual(TEXT("readback property canonicalization preserves Description"),
			Entry.Elements[1].StringValue, FString(TEXT("Canonical properties")))
		&& Test.TestEqual(TEXT("readback property canonicalization sorts property names"),
			Properties.ObjectKeys, TArray<FString>{TEXT("Int32Value"), TEXT("StringValue")})
		&& Test.TestTrue(TEXT("readback property canonicalization has two property values"),
			Properties.Elements.Num() == 2)
		&& Test.TestEqual(TEXT("readback property canonicalization preserves integer kind/value"),
			Properties.Elements.IsValidIndex(0) ? Properties.Elements[0].IntegerValue : 0, 41ll)
		&& Test.TestEqual(TEXT("readback property canonicalization preserves string kind/value"),
			Properties.Elements.IsValidIndex(1) ? Properties.Elements[1].StringValue : FString(),
			FString(TEXT("Canonical value")))
		&& Test.TestEqual(TEXT("readback property canonicalization preserves local name"),
			Entry.Elements[4].StringValue, FString(TEXT("PropertyCanonical")))
		&& Test.TestTrue(TEXT("readback property canonicalization preserves true bInstanceSynced"),
			Entry.Elements[5].BooleanValue);
}

bool RequireCanonicalReference(
	FAutomationTestBase& Test,
	const FString& Label,
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Kind,
	const FString& Path)
{
	return Test.TestEqual(*(Label + TEXT(" uses a two-field reference object")), Value.Kind,
			EAssetDocumentRuntimeValueKind::Object)
		&& Test.TestEqual(*(Label + TEXT(" reference fields are canonical")), Value.ObjectKeys,
			TArray<FString>{TEXT("Kind"), TEXT("Path")})
		&& Test.TestTrue(*(Label + TEXT(" has exactly two reference values")), Value.Elements.Num() == 2)
		&& Test.TestEqual(*(Label + TEXT(" reference kind is canonical")),
			Value.Elements.IsValidIndex(0) ? Value.Elements[0].StringValue : FString(), FString(Kind))
		&& Test.TestEqual(*(Label + TEXT(" reference path is canonical")),
			Value.Elements.IsValidIndex(1) ? Value.Elements[1].StringValue : FString(), Path);
}

bool RequireCanonicalSpecialKeyReadback(
	FAutomationTestBase& Test,
	const FString& Label,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const UClass& KeyTypeClass,
	const FString& Name,
	const TArray<FString>& ExpectedPropertyNames)
{
	if (!RequireSuccess(Test, Label, Outcome)
		|| !Test.TestEqual(*(Label + TEXT(" has only entry")), Outcome.Value.ObjectKeys,
			TArray<FString>{TEXT("entry")})
		|| !Test.TestTrue(*(Label + TEXT(" has one entry value")), Outcome.Value.Elements.Num() == 1))
	{
		return false;
	}

	const FAssetDocumentRuntimeValueDto& Entry = Outcome.Value.Elements[0];
	if (!Test.TestEqual(*(Label + TEXT(" has the complete six-field entry shape")), Entry.ObjectKeys,
		TArray<FString>{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")})
		|| !Test.TestTrue(*(Label + TEXT(" has exactly six entry values")), Entry.Elements.Num() == 6)
		|| !RequireCanonicalReference(Test, Label + TEXT(" key type"), Entry.Elements[2], TEXT("ClassRef"),
			KeyTypeClass.GetPathName()))
	{
		return false;
	}

	const FAssetDocumentRuntimeValueDto& Properties = Entry.Elements[3];
	return Test.TestEqual(*(Label + TEXT(" preserves empty Category")), Entry.Elements[0].StringValue, FString())
		&& Test.TestEqual(*(Label + TEXT(" preserves empty Description")), Entry.Elements[1].StringValue, FString())
		&& Test.TestEqual(*(Label + TEXT(" sorts all admitted property names")),
			Properties.ObjectKeys, ExpectedPropertyNames)
		&& Test.TestTrue(*(Label + TEXT(" has one value per admitted property")),
			Properties.Elements.Num() == ExpectedPropertyNames.Num())
		&& Test.TestEqual(*(Label + TEXT(" preserves the owned-local name")), Entry.Elements[4].StringValue, Name)
		&& Test.TestFalse(*(Label + TEXT(" preserves false bInstanceSynced")), Entry.Elements[5].BooleanValue)
		&& Test.TestTrue(*(Label + TEXT(" has no handles or changed set")),
			Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
}

class FScopedKeyTypeClassFlags final
{
public:
	FScopedKeyTypeClassFlags(UClass& InClass, const EClassFlags AdditionalFlags)
		: Class(InClass)
		, OriginalFlags(InClass.ClassFlags)
	{
		Class.ClassFlags |= AdditionalFlags;
	}

	~FScopedKeyTypeClassFlags()
	{
		Class.ClassFlags = OriginalFlags;
	}

private:
	UClass& Class;
	EClassFlags OriginalFlags;
};

bool RequireExactChangedSet(FAutomationTestBase& Test, const FString& Label,
	const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Authority, const FString& AssetPath,
	const FString& Path)
{
	if (!Test.TestEqual(*(Label + TEXT(" emits one exact ChangedSet record")), Outcome.ChangedSet.Num(), 1)
		|| Outcome.ChangedSet.Num() != 1)
	{
		return false;
	}
	const FAssetDocumentChangedRecord& Change = Outcome.ChangedSet[0];
	return Test.TestEqual(*(Label + TEXT(" ChangedSet authority")), Change.AuthorityId, FString(Authority))
		&& Test.TestEqual(*(Label + TEXT(" ChangedSet asset")), Change.StableObjectId, AssetPath)
		&& Test.TestEqual(*(Label + TEXT(" ChangedSet path")), Change.Path, Path)
		&& Test.TestEqual(*(Label + TEXT(" ChangedSet kind")), Change.ChangeKind, FString(TEXT("Update")));
}

class FScopedBlackboardTestFailurePoint final
{
public:
	explicit FScopedBlackboardTestFailurePoint(const EAssetDocumentAIBlackboardTestFailurePoint InPoint)
	{
		SetAssetDocumentAIBlackboardTestFailurePoint(InPoint);
	}
	~FScopedBlackboardTestFailurePoint()
	{
		SetAssetDocumentAIBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::None);
	}
};

class FScopedLoadedConsumerCompletionTestFailurePoint final
{
public:
	explicit FScopedLoadedConsumerCompletionTestFailurePoint(
		const AssetDocumentAIBlackboardLoadedConsumerCompletion::
			ELoadedConsumerCompletionTestFailurePoint InPoint)
	{
		AssetDocumentAIBlackboardLoadedConsumerCompletion::
			SetLoadedConsumerCompletionTestFailurePoint(InPoint);
	}
	~FScopedLoadedConsumerCompletionTestFailurePoint()
	{
		AssetDocumentAIBlackboardLoadedConsumerCompletion::
			SetLoadedConsumerCompletionTestFailurePoint(
				AssetDocumentAIBlackboardLoadedConsumerCompletion::
					ELoadedConsumerCompletionTestFailurePoint::None);
	}
};

class FScopedLifecycleRegisteredAsset final
{
public:
	explicit FScopedLifecycleRegisteredAsset(UObject& InAsset)
		: Asset(&InAsset)
	{
		FAssetRegistryModule::AssetCreated(&InAsset);
	}

	~FScopedLifecycleRegisteredAsset()
	{
		if (UObject* Existing = Asset.Get())
		{
			FAssetRegistryModule::AssetDeleted(Existing);
		}
	}

private:
	TWeakObjectPtr<UObject> Asset;
};

struct FRemoveLoadedConsumerFixture final
{
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UBehaviorTree> Tree;
	TStrongObjectPtr<UBehaviorTreeGraph> Graph;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Root> Root;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Composite> Composite;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Task> TaskGraph;
	TStrongObjectPtr<UAssetDocumentAIBlackboardConsumerCompletionTestTask> Task;
	TUniquePtr<FScopedLifecycleRegisteredAsset> Registration;
};

UEdGraphPin* FindRemoveConsumerPin(
	const UEdGraphNode& Node, const EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && Pin->Direction == Direction)
		{
			return Pin;
		}
	}
	return nullptr;
}

void ConfigureRemoveConsumerIntSelector(FBlackboardKeySelector& Selector,
	UObject& Owner, const FName PropertyName, const FName KeyName)
{
	Selector.SelectedKeyName = KeyName;
	Selector.AddIntFilter(&Owner, PropertyName);
}

bool ConfigureRemoveLoadedConsumerTask(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task,
	const FName RetainedName, const FName RemovedName)
{
	ConfigureRemoveConsumerIntSelector(Task.DirectSelector, Task,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAIBlackboardConsumerCompletionTestTask, DirectSelector),
		RetainedName);
	Task.ValueOrBool = FValueOrBBKey_Bool(false);
	Task.ValueOrClass = FValueOrBBKey_Class();
	Task.ValueOrClass.SetBaseClass(UObject::StaticClass());
	Task.ValueOrEnum = FValueOrBBKey_Enum();
	Task.ValueOrEnum.SetEnumType(StaticEnum<EBasicKeyOperation::Type>());
	Task.ValueOrFloat = FValueOrBBKey_Float(1.25f);
	Task.ValueOrInt32 = FValueOrBBKey_Int32(37);
	Task.ValueOrInt32.SetKey(RemovedName);
	Task.ValueOrName = FValueOrBBKey_Name(TEXT("RemoveDefaultName"));
	Task.ValueOrString = FValueOrBBKey_String(TEXT("RemoveDefaultString"));
	Task.ValueOrObject = FValueOrBBKey_Object();
	Task.ValueOrObject.SetBaseClass(UObject::StaticClass());
	Task.ValueOrRotator = FValueOrBBKey_Rotator(FRotator(1.0, 2.0, 3.0));
	Task.ValueOrStruct = FValueOrBBKey_Struct(FVector(2.0, 3.0, 5.0));
	Task.ValueOrStruct.SetStructType(TBaseStructure<FVector>::Get());
	Task.ValueOrVector = FValueOrBBKey_Vector(FVector(5.0, 8.0, 13.0));

	ConfigureRemoveConsumerIntSelector(Task.Matrix.NestedSelector, Task,
		GET_MEMBER_NAME_CHECKED(
			FAssetDocumentAIBlackboardConsumerCompletionTestMatrix, NestedSelector),
		RetainedName);
	Task.Matrix.NestedValueOrInt32 = FValueOrBBKey_Int32(71);
	Task.Matrix.NestedInstancedValue.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue>();
	auto& Instanced = Task.Matrix.NestedInstancedValue.GetMutable<
		FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue>();
	ConfigureRemoveConsumerIntSelector(Instanced.Selector, Task,
		GET_MEMBER_NAME_CHECKED(
			FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue, Selector),
		RetainedName);
	Instanced.ValueOrName = FValueOrBBKey_Name(TEXT("RemoveInstancedDefault"));

	FBlackboardKeySelector ArraySelector;
	ConfigureRemoveConsumerIntSelector(ArraySelector, Task,
		GET_MEMBER_NAME_CHECKED(
			FAssetDocumentAIBlackboardConsumerCompletionTestMatrix, UniqueSelectors),
		RetainedName);
	Task.Matrix.UniqueSelectors = {MoveTemp(ArraySelector)};
	FBlackboardKeySelector MapSelector;
	ConfigureRemoveConsumerIntSelector(MapSelector, Task,
		GET_MEMBER_NAME_CHECKED(
			FAssetDocumentAIBlackboardConsumerCompletionTestMatrix, StableSelectorValues),
		RetainedName);
	Task.Matrix.StableSelectorValues.Add(TEXT("RemoveStable"), MoveTemp(MapSelector));
	Task.ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionEmptyContainer>();
	Task.CaptureBudgetStringA = FValueOrBBKey_String(TEXT("RemoveBudgetDefaultA"));
	Task.CaptureBudgetStringB = FValueOrBBKey_String(TEXT("RemoveBudgetDefaultB"));
	return Test.TestTrue(TEXT("Remove consumer uses exact nested fixture type"),
		Task.Matrix.NestedInstancedValue.GetScriptStruct()
			== FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue::StaticStruct());
}

TUniquePtr<FRemoveLoadedConsumerFixture> MakeRemoveLoadedConsumerFixture(
	FAutomationTestBase& Test, UBlackboardData& Blackboard,
	const FName RetainedName, const FName RemovedName)
{
	TUniquePtr<FRemoveLoadedConsumerFixture> Fixture =
		MakeUnique<FRemoveLoadedConsumerFixture>();
	Fixture->Package.Reset(CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2RemoveConsumer_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	Fixture->Tree.Reset(NewObject<UBehaviorTree>(Fixture->Package.Get(),
		TEXT("BT_RemoveConsumer"), RF_Public | RF_Standalone | RF_Transactional));
	Fixture->Graph.Reset(Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(
		Fixture->Tree.Get(), TEXT("BehaviorTreeGraph"), UBehaviorTreeGraph::StaticClass(),
		UEdGraphSchema_BehaviorTree::StaticClass())));
	if (!Test.TestNotNull(TEXT("Remove consumer package exists"), Fixture->Package.Get())
		|| !Test.TestNotNull(TEXT("Remove consumer tree exists"), Fixture->Tree.Get())
		|| !Test.TestNotNull(TEXT("Remove consumer graph exists"), Fixture->Graph.Get()))
	{
		return nullptr;
	}

	Fixture->Tree->BTGraph = Fixture->Graph.Get();
	Fixture->Tree->BlackboardAsset = &Blackboard;
	Fixture->Graph->GetSchema()->CreateDefaultNodesForGraph(*Fixture->Graph);
	Fixture->Graph->OnCreated();
	Fixture->Graph->Initialize();
	Fixture->Root.Reset(Fixture->Graph->Nodes.Num() == 1
		? Cast<UBehaviorTreeGraphNode_Root>(Fixture->Graph->Nodes[0]) : nullptr);
	if (!Test.TestNotNull(TEXT("Remove consumer root exists"), Fixture->Root.Get()))
	{
		return nullptr;
	}
	Fixture->Root->BlackboardAsset = &Blackboard;
	Fixture->Root->UpdateBlackboard();

	FAISchemaAction_NewNode CompositeAction;
	CompositeAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Composite>();
	CompositeAction.NodeTemplate->NodeInstance =
		NewObject<UBTComposite_Sequence>(CompositeAction.NodeTemplate);
	Fixture->Composite.Reset(Cast<UBehaviorTreeGraphNode_Composite>(
		CompositeAction.PerformAction(Fixture->Graph.Get(),
			FindRemoveConsumerPin(*Fixture->Root, EGPD_Output),
			FVector2f(160.0f, 0.0f), false)));
	FAISchemaAction_NewNode TaskAction;
	TaskAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Task>();
	TaskAction.NodeTemplate->NodeInstance =
		NewObject<UAssetDocumentAIBlackboardConsumerCompletionTestTask>(
			TaskAction.NodeTemplate);
	Fixture->TaskGraph.Reset(Cast<UBehaviorTreeGraphNode_Task>(
		TaskAction.PerformAction(Fixture->Graph.Get(),
			Fixture->Composite.IsValid()
				? FindRemoveConsumerPin(*Fixture->Composite, EGPD_Output) : nullptr,
			FVector2f(320.0f, 0.0f), false)));
	Fixture->Task.Reset(Fixture->TaskGraph.IsValid()
		? Cast<UAssetDocumentAIBlackboardConsumerCompletionTestTask>(
			Fixture->TaskGraph->NodeInstance) : nullptr);
	if (!Test.TestNotNull(TEXT("Remove consumer composite exists"), Fixture->Composite.Get())
		|| !Test.TestNotNull(TEXT("Remove consumer task graph exists"), Fixture->TaskGraph.Get())
		|| !Test.TestNotNull(TEXT("Remove consumer task exists"), Fixture->Task.Get())
		|| !ConfigureRemoveLoadedConsumerTask(
			Test, *Fixture->Task, RetainedName, RemovedName))
	{
		return nullptr;
	}
	Fixture->Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
	if (!Test.TestEqual(TEXT("Remove consumer task belongs to exact tree"),
		Fixture->Task->GetTreeAsset(), Fixture->Tree.Get()))
	{
		return nullptr;
	}
	Fixture->Registration =
		MakeUnique<FScopedLifecycleRegisteredAsset>(*Fixture->Tree);
	Fixture->Package->SetDirtyFlag(false);
	return Fixture;
}

TStrongObjectPtr<UBlackboardComponent> MakeRemoveConsumerComponent(
	FAutomationTestBase& Test, UBlackboardData& Blackboard)
{
	UWorld* World = GEditor != nullptr
		? GEditor->GetEditorWorldContext().World() : nullptr;
	TStrongObjectPtr<UBlackboardComponent> Component(World != nullptr
		? NewObject<UBlackboardComponent>(World) : nullptr);
	if (!Test.TestNotNull(TEXT("Remove consumer has Editor world"), World)
		|| !Test.TestTrue(TEXT("Remove consumer initializes Blackboard component"),
			Component.IsValid() && Component->InitializeBlackboard(Blackboard)))
	{
		return {};
	}
	return Component;
}

bool PrimeRemoveLoadedConsumer(FAutomationTestBase& Test,
	FRemoveLoadedConsumerFixture& Fixture, UBlackboardData& Blackboard,
	const FName RetainedName, const FName RemovedName, const int32 RuntimeValue)
{
	Fixture.Task->DirectSelector.InvalidateResolvedKey();
	Fixture.Task->DirectSelector.ResolveSelectedKey(Blackboard);
	TStrongObjectPtr<UBlackboardComponent> Component =
		MakeRemoveConsumerComponent(Test, Blackboard);
	if (!Component.IsValid()) return false;
	Component->SetValueAsInt(RemovedName, RuntimeValue);
	return Test.TestEqual(TEXT("Remove consumer primes retained selector Key-ID"),
		Fixture.Task->DirectSelector.GetSelectedKeyID(), Blackboard.GetKeyID(RetainedName))
		&& Test.TestEqual(TEXT("Remove consumer primes retained selector type"),
			Fixture.Task->DirectSelector.SelectedKeyType.Get(),
			UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass())
		&& Test.TestEqual(TEXT("Remove consumer primes ValueOr runtime cache"),
			Fixture.Task->ValueOrInt32.GetValue(Component.Get()), RuntimeValue);
}

bool RequireRemoveLoadedConsumerForward(FAutomationTestBase& Test,
	FRemoveLoadedConsumerFixture& Fixture, UBlackboardData& Blackboard,
	const FName RetainedName, const FName RemovedName,
	const FBlackboard::FKey BeforeRetainedId, const bool bBeforeTreeDirty)
{
	TStrongObjectPtr<UBlackboardComponent> Component =
		MakeRemoveConsumerComponent(Test, Blackboard);
	if (!Component.IsValid()) return false;
	return Test.TestEqual(TEXT("Remove consumer resolves retained selector after removal"),
		Fixture.Task->DirectSelector.GetSelectedKeyID(), Blackboard.GetKeyID(RetainedName))
		&& Test.TestNotEqual(TEXT("Remove consumer observes retained Key-ID movement"),
			Fixture.Task->DirectSelector.GetSelectedKeyID(), BeforeRetainedId)
		&& Test.TestEqual(TEXT("Remove consumer invalidates deleted ValueOr to authored default"),
			Fixture.Task->ValueOrInt32.GetValue(Component.Get()), 37)
		&& Test.TestEqual(TEXT("Remove consumer keeps deleted key absent"),
			Blackboard.GetKeyID(RemovedName), FBlackboard::InvalidKey)
		&& Test.TestEqual(TEXT("Remove consumer preserves tree dirty state after forward completion"),
			Fixture.Tree->GetOutermost()->IsDirty(), bBeforeTreeDirty);
}

bool RequireRemoveLoadedConsumerRollback(FAutomationTestBase& Test,
	FRemoveLoadedConsumerFixture& Fixture, UBlackboardData& Blackboard,
	const FName RetainedName, const FName RemovedName,
	const FBlackboard::FKey BeforeRetainedId,
	const FBlackboard::FKey BeforeRemovedId, const bool bBeforeTreeDirty,
	const int32 RuntimeValue)
{
	TStrongObjectPtr<UBlackboardComponent> Component =
		MakeRemoveConsumerComponent(Test, Blackboard);
	if (!Component.IsValid()) return false;
	Component->SetValueAsInt(RemovedName, RuntimeValue);
	return Test.TestEqual(TEXT("Remove rollback restores retained selector Key-ID"),
		Fixture.Task->DirectSelector.GetSelectedKeyID(), BeforeRetainedId)
		&& Test.TestEqual(TEXT("Remove rollback restores retained Blackboard Key-ID"),
			Blackboard.GetKeyID(RetainedName), BeforeRetainedId)
		&& Test.TestEqual(TEXT("Remove rollback restores removed Blackboard Key-ID"),
			Blackboard.GetKeyID(RemovedName), BeforeRemovedId)
		&& Test.TestEqual(TEXT("Remove rollback restores ValueOr runtime resolution"),
			Fixture.Task->ValueOrInt32.GetValue(Component.Get()), RuntimeValue)
		&& Test.TestEqual(TEXT("Remove rollback preserves tree dirty state"),
			Fixture.Tree->GetOutermost()->IsDirty(), bBeforeTreeDirty);
}

class FScopedBlackboardMetadataWriteTestFailurePoint final
{
public:
	explicit FScopedBlackboardMetadataWriteTestFailurePoint(
		const AssetDocumentAIBlackboardMutation::EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint InPoint,
		const int32 KeyTypeLeafIndex = INDEX_NONE)
	{
		AssetDocumentAIBlackboardMutation::SetBlackboardMetadataWriteTestFailurePointInternal(
			InPoint, KeyTypeLeafIndex);
	}
	~FScopedBlackboardMetadataWriteTestFailurePoint()
	{
		AssetDocumentAIBlackboardMutation::SetBlackboardMetadataWriteTestFailurePointInternal(
			AssetDocumentAIBlackboardMutation::EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::None,
			INDEX_NONE);
	}
};

} // namespace AssetDocumentAIBlackboardLifecycleTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardStructuralPublicationOwnershipTest,
	"UEAssetDoc.AIProvider.BlackboardV2.StructuralPublicationOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardStructuralPublicationOwnershipTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardStructuralPublication;
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior: staging ownership follows the real Atomic runtime terminal, not
	//   the caller binding or inner rollback callback.
	// - Technique: state-transition testing plus the zero/one boundary for staged
	//   object ownership.
	// - Test: real Commit and Rollback release only when the runtime destroys
	//   its action; rejected publication and double-stage retain exactly one owner.
	TStrongObjectPtr<UBlackboardData> Blackboard(NewObject<UBlackboardData>(
		GetTransientPackage(), NAME_None, RF_Transactional));
	TStrongObjectPtr<UBlackboardData> OtherBlackboard(NewObject<UBlackboardData>(
		GetTransientPackage(), NAME_None, RF_Transactional));
	if (!TestTrue(TEXT("test Blackboards exist"), Blackboard.IsValid() && OtherBlackboard.IsValid()))
	{
		return false;
	}
	Blackboard->UpdateParentKeys();
	const TArray<FBlackboardEntry> ExactPreimage = Blackboard->Keys;
	FStructuralPublicationRealAtomicScenario RollbackScenario;
	if (!RollbackScenario.Begin(*this, *Blackboard))
	{
		return false;
	}

	const FString ComponentId = TEXT("assetdocument.ai.blackboard.local_keys.v1");
	FStagedKeyTypeResult AdoptedStage;
	TWeakObjectPtr<UBlackboardKeyType> AdoptedWeak;
	{
		FStructuralPublicationTransactionBinding TransactionBinding(
			*Blackboard, ComponentId, *RollbackScenario.Transaction);
		TestEqual(TEXT("binding exposes the exact active Atomic transaction identity"),
			TransactionBinding.GetTransactionId(),
			RollbackScenario.Transaction->GetTransactionId());
		FStagedKeyTypeResult MissingFootprint = StageKeyType(
			*Blackboard, UBlackboardKeyType_Bool::StaticClass(), TransactionBinding);
		TestEqual(TEXT("missing Atomic capture/footprint returns typed failure"),
			MissingFootprint.Failure, EStageKeyTypeFailure::MissingTransactionPreconditions);
		const FAssetDocumentEditorTypedPreimageResult Capture =
			CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
				*RollbackScenario.Lease, *Blackboard, {ComponentId}, TransactionBinding);
		if (!TestTrue(TEXT("real typed preimage capture enlists through binding"),
			Capture.bSucceeded)) return false;
		if (!TestTrue(TEXT("real Atomic footprint enlists through binding"),
			TransactionBinding.EnlistFootprint(
				{RollbackScenario.Footprint}).bValid)) return false;
		FStagedKeyTypeResult WrongTarget = StageKeyType(
			*OtherBlackboard, UBlackboardKeyType_Bool::StaticClass(), TransactionBinding);
		TestEqual(TEXT("mismatched exact target returns typed failure"),
			WrongTarget.Failure, EStageKeyTypeFailure::InvalidTarget);
		FStagedKeyTypeResult WrongClass = StageKeyType(
			*Blackboard, UObject::StaticClass(), TransactionBinding);
		TestEqual(TEXT("non-KeyType class returns typed failure"),
			WrongClass.Failure, EStageKeyTypeFailure::InvalidKeyTypeClass);

		AdoptedStage = StageKeyType(
			*Blackboard, UBlackboardKeyType_Bool::StaticClass(), TransactionBinding);
		UBlackboardKeyType* AdoptedKeyType = AdoptedStage.Get();
		if (!TestNotNull(TEXT("valid stage is constructed"), AdoptedKeyType)) return false;
		AdoptedWeak = AdoptedKeyType;
		TestTrue(TEXT("real transaction action strongly owns staged KeyType"),
			AdoptedStage.HasStrongOwnership());
		TestTrue(TEXT("staged object uses exact target Outer and flags"),
			AdoptedKeyType->GetOuter() == Blackboard.Get()
				&& AdoptedKeyType->HasAnyFlags(RF_Transactional)
				&& !AdoptedKeyType->HasAnyFlags(RF_Public | RF_Standalone));
		TestTrue(TEXT("staged object name uses the frozen collision-free prefix"),
			AdoptedKeyType->GetName().StartsWith(
				TEXT("AssetDocumentAIBlackboardStagedKeyType")));
		const int32 OwnedKeyTypeCount = CountDirectBlackboardKeyTypes(*Blackboard);
		const FStagedKeyTypeResult DuplicateStage = StageKeyType(
			*Blackboard, UBlackboardKeyType_Bool::StaticClass(), TransactionBinding);
		TestEqual(TEXT("double-stage fails before construction"),
			DuplicateStage.Failure, EStageKeyTypeFailure::ConstructionFailed);
		TestEqual(TEXT("double-stage creates no unowned raw UObject"),
			CountDirectBlackboardKeyTypes(*Blackboard), OwnedKeyTypeCount);

		TSharedPtr<IPropertyRowGenerator> KeysRows;
		const TSharedPtr<IPropertyHandle> KeysHandle =
			MakeExactKeysHandle(*this, *Blackboard, KeysRows);
		if (!KeysHandle.IsValid()) return false;
		TArray<FBlackboardEntry> Published = ExactPreimage;
		FBlackboardEntry Entry;
		Entry.EntryName = TEXT("Published");
		Entry.KeyType = AdoptedKeyType;
		Published.Insert(Entry, 0);
		const TArray<FBlackboardEntry> BeforeRejectedHandle = Blackboard->Keys;
		TestFalse(TEXT("snapshot omitting staged pointer fails closed"),
			PublishLocalKeysSnapshot(
				*Blackboard, ExactPreimage, TransactionBinding, KeysHandle));
		TestTrue(TEXT("rejected publication retains transaction strong ownership"),
			AdoptedStage.HasStrongOwnership());
		TestEqual(TEXT("rejected publication preserves exact Keys snapshot"),
			Blackboard->Keys, BeforeRejectedHandle);
		TestFalse(TEXT("null/wrong Keys handle performs zero write"),
			PublishLocalKeysSnapshot(
				*Blackboard, Published, TransactionBinding, TSharedPtr<IPropertyHandle>()));
		TSharedPtr<IPropertyRowGenerator> OtherRows;
		const TSet<FString> KeysAllowList = {
			GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString()};
		if (!TestTrue(TEXT("wrong-outer Keys rows are available"),
			AssetDocumentAIBlackboardMutation::TryMakeRows(
				*OtherBlackboard, OtherRows, &KeysAllowList))) return false;
		const TSharedPtr<IPropertyHandle> WrongOuterKeys =
			AssetDocumentAIBlackboardMutation::FindUniqueOwnedPropertyHandle(
				*OtherRows, *OtherBlackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
		TestFalse(TEXT("same Keys property with wrong unique outer performs zero write"),
			PublishLocalKeysSnapshot(
				*Blackboard, Published, TransactionBinding, WrongOuterKeys));
		TestEqual(TEXT("rejected handles preserve exact Keys snapshot"),
			Blackboard->Keys, BeforeRejectedHandle);
		if (!TestTrue(TEXT("exact publication succeeds"), PublishLocalKeysSnapshot(
			*Blackboard, Published, TransactionBinding, KeysHandle))) return false;
		TestTrue(TEXT("published stage stays transaction-owned until terminal"),
			AdoptedStage.HasStrongOwnership());
		const TSharedPtr<IPropertyHandleArray> KeysArray = KeysHandle->AsArray();
		uint32 ElementCount = 0;
		if (!TestTrue(TEXT("Keys array handle exposes a nested element"),
			KeysArray.IsValid()
				&& KeysArray->GetNumElements(ElementCount) == FPropertyAccess::Success
				&& ElementCount > 0)) return false;
		const TArray<FBlackboardEntry> BeforeRejectedNestedHandle = Blackboard->Keys;
		TestFalse(TEXT("nested Keys element handle performs zero write/notification"),
			PublishLocalKeysSnapshot(
				*Blackboard, Published, TransactionBinding, KeysArray->GetElement(0)));
		TestEqual(TEXT("nested rejected handle preserves published Keys snapshot"),
			Blackboard->Keys, BeforeRejectedNestedHandle);
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("caller binding destruction does not release transaction-owned stage"),
		AdoptedWeak.IsValid() && AdoptedStage.HasStrongOwnership());
	if (!TestTrue(TEXT("real outer Atomic rollback succeeds"),
		RollbackScenario.Transaction->Rollback(
			TEXT("test.structural-publication")).bValid)) return false;
	TestEqual(TEXT("typed-preimage rollback restores the exact Keys preimage"),
		Blackboard->Keys, ExactPreimage);
	TestTrue(TEXT("stage records the typed-preimage rollback verification terminal"),
		AdoptedStage.HasRollbackVerifiedTerminal());
	TestTrue(TEXT("inner typed-preimage rollback retained strong ownership"),
		AdoptedStage.WasStrongOwnershipRetainedDuringRollback());
	TestTrue(TEXT("real rollback terminal destroyed the transaction action"),
		AdoptedStage.HasTransactionTerminalReleasedOwnership());
	TestFalse(TEXT("real rollback terminal releases transaction strong ownership"),
		AdoptedStage.HasStrongOwnership());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("rolled-back staged KeyType has no leak"), AdoptedWeak.IsValid());

	TStrongObjectPtr<UBlackboardData> CommitBlackboard(NewObject<UBlackboardData>(
		GetTransientPackage(), NAME_None, RF_Transactional));
	if (!TestTrue(TEXT("commit Blackboard exists"), CommitBlackboard.IsValid()))
	{
		return false;
	}
	CommitBlackboard->UpdateParentKeys();
	const TArray<FBlackboardEntry> CommitPreimage = CommitBlackboard->Keys;
	FStructuralPublicationRealAtomicScenario CommitScenario;
	if (!CommitScenario.Begin(*this, *CommitBlackboard))
	{
		return false;
	}
	FStagedKeyTypeResult CommittedStage;
	TWeakObjectPtr<UBlackboardKeyType> CommittedWeak;
	{
		FStructuralPublicationTransactionBinding TransactionBinding(
			*CommitBlackboard, ComponentId, *CommitScenario.Transaction);
		const FAssetDocumentEditorTypedPreimageResult Capture =
			CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
				*CommitScenario.Lease, *CommitBlackboard,
				{ComponentId}, TransactionBinding);
		if (!TestTrue(TEXT("commit lane captures real typed preimage"),
			Capture.bSucceeded)
			|| !TestTrue(TEXT("commit lane enlists exact real footprint"),
				TransactionBinding.EnlistFootprint(
					{CommitScenario.Footprint}).bValid))
		{
			return false;
		}
		CommittedStage = StageKeyType(
			*CommitBlackboard, UBlackboardKeyType_Bool::StaticClass(),
			TransactionBinding);
		UBlackboardKeyType* CommittedKeyType = CommittedStage.Get();
		if (!TestNotNull(TEXT("commit lane constructs stage"), CommittedKeyType))
		{
			return false;
		}
		CommittedWeak = CommittedKeyType;
		TArray<FBlackboardEntry> Published = CommitPreimage;
		FBlackboardEntry Entry;
		Entry.EntryName = TEXT("Committed");
		Entry.KeyType = CommittedKeyType;
		Published.Insert(Entry, 0);
		TSharedPtr<IPropertyRowGenerator> KeysRows;
		const TSharedPtr<IPropertyHandle> KeysHandle =
			MakeExactKeysHandle(*this, *CommitBlackboard, KeysRows);
		if (!KeysHandle.IsValid()
			|| !TestTrue(TEXT("commit lane publishes exact staged pointer"),
				PublishLocalKeysSnapshot(
					*CommitBlackboard, Published, TransactionBinding, KeysHandle)))
		{
			return false;
		}
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("commit lane binding scope release retains transaction owner"),
		CommittedWeak.IsValid() && CommittedStage.HasStrongOwnership());
	if (!TestTrue(TEXT("real Atomic commit lane marks ready"),
		CommitScenario.Transaction->MarkReadyToCommit().bValid)
		|| !TestTrue(TEXT("real Atomic commit succeeds"),
			CommitScenario.Transaction->Commit(
				MakeStructuralCompletionReceipt(
					*CommitScenario.Transaction)).bValid))
	{
		return false;
	}
	TestEqual(TEXT("real Atomic chooses committed terminal"),
		CommitScenario.Transaction->GetState(), EAssetDocumentAtomicState::Committed);
	TestTrue(TEXT("real commit terminal destroyed the transaction action"),
		CommittedStage.HasTransactionTerminalReleasedOwnership());
	TestFalse(TEXT("real commit terminal releases transaction strong ownership"),
		CommittedStage.HasStrongOwnership());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("committed exact pointer remains published after owner release"),
		CommittedWeak.IsValid()
			&& CommitBlackboard->Keys.Num() > 0
			&& CommitBlackboard->Keys[0].KeyType.Get() == CommittedWeak.Get());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardLifecycleTest,
	"UEAssetDoc.AIProvider.BlackboardLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardLifecycleTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior: each ordinary Blackboard row consumes an existing Editor
	//   payload through an AI-scoped target handle and leaves an atomic, exact
	//   authored/derived rollback boundary.
	// - Technique: lifecycle state transitions plus equivalence/failure boundaries.
	// - Risk: accepting a direct Editor handle, enlisting footprint before action,
	//   or replacing existing KeyType instances during an order-only change.
	// - Observations: authoritative write, exact ChangedSet, and exact
	//   shared-action rollback state are all externally observable.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardLifecycle_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Parent = NewObject<UBlackboardData>(
		Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("test-owned Blackboard exists"), Blackboard)
		|| !TestNotNull(TEXT("test-owned parent Blackboard exists"), Parent)
		|| !TestNotNull(TEXT("test-owned child Blackboard exists"), Child)) return false;
	// Project persistent-key delegates run from UBlackboardData::PostInitProperties.
	// The focused fixture keeps those derived rows byte-identical and appends
	// them after the explicitly authored local rows, matching provider semantics.
	Blackboard->UpdateParentKeys();
	Parent->UpdateParentKeys();
	TArray<FBlackboardEntry> BlackboardDerivedKeys = MoveTemp(Blackboard->Keys);
	TArray<FBlackboardEntry> ParentDerivedKeys = MoveTemp(Parent->Keys);
	Blackboard->Keys.Reset();
	Parent->Keys.Reset();
	AddExistingKey(*Blackboard, TEXT("ExistingKey"), 7);
	AddExistingKey(*Blackboard, TEXT("SecondKey"), 13);
	AddExistingKey(*Parent, TEXT("InheritedKey"), 19);
	Blackboard->Keys.Append(MoveTemp(BlackboardDerivedKeys));
	Parent->Keys.Append(MoveTemp(ParentDerivedKeys));
	Blackboard->UpdateParentKeys();
	Parent->UpdateParentKeys();
	if (!TestTrue(TEXT("fixture has one exact native SelfActor at the local tail"),
		Blackboard->Keys.Num() == 3 && IsEngineDerivedSelfKey(Blackboard->Keys.Last())))
	{
		return false;
	}
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();

	bool Passed = true;
	auto MakeAIHandle = [&](FBlackboardLifecycleTransaction& Transaction,
		FBlackboardLifecycleHandleResolver& Handles) -> TOptional<FAssetDocumentOpaqueHandle>
	{
		const TOptional<FAssetDocumentOpaqueHandle> Editor = ResolveEditorMutableAsset(
			*this, *Blackboard, Transaction.GetMutationToken(), Handles);
		return Editor.IsSet() ? Handles.AddProviderLocalAIResult(Editor.GetValue()) : TOptional<FAssetDocumentOpaqueHandle>();
	};

	// Handle-free inspect has no transaction/handle shortcut and reads the existing owned key.
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeInvocation(KeyTypeInspectId, Handle.GetValue(), *Blackboard, EAssetDocumentRuntimeRequestPhase::Validate),
			Handles, nullptr);
		Passed &= RequireSuccess(*this, TEXT("handle-free existing key inspect"), Outcome);
		Passed &= TestEqual(TEXT("inspect returns strict lexicographic object keys"), Outcome.Value.ObjectKeys,
			TArray<FString>{TEXT("canonical_properties"), TEXT("class_path"), TEXT("property_schema")});
		Passed &= TestTrue(TEXT("inspect reads ExistingKey admitted Int32Value"),
			Outcome.Value.Elements.IsValidIndex(0) && Outcome.Value.Elements[0].Kind == EAssetDocumentRuntimeValueKind::Object
			&& Outcome.Value.Elements[0].ObjectKeys.Contains(TEXT("Int32Value")));
	}

	// Direct Editor metadata must reject before the AI handler; the test-local
	// target result retains token, provider/kind/access metadata and payload identity.
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Editor = ResolveEditorMutableAsset(
			*this, *Blackboard, Transaction.GetMutationToken(), Handles);
		if (!Editor.IsSet()) return false;
		const TOptional<FAssetDocumentOpaqueHandle> AI = Handles.AddProviderLocalAIResult(Editor.GetValue());
		if (!TestTrue(TEXT("provider-local AI target fixture exists"), AI.IsSet())) return false;
		FAssetDocumentHandleValidationContext AIContext;
		AIContext.RequestId = Transaction.GetMutationToken().RequestId;
		AIContext.TransactionId = Transaction.GetMutationToken().TransactionId;
		AIContext.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
		AIContext.ProviderId = TEXT("ai");
		AIContext.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
		AIContext.RequiredKind = EAssetDocumentHandleKind::Asset;
		AIContext.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
		const FAssetDocumentVerifiedLeaseResult Foreign = Handles.AcquireVerifiedLease(Editor.GetValue(), AIContext);
		Passed &= TestNull(TEXT("direct Editor handle is rejected for AI"), Foreign.Lease.Get());
		Passed &= TestTrue(TEXT("direct Editor handle preserves ProviderMismatch"), Foreign.Diagnostics.ContainsByPredicate(
			[](const FAssetDocumentRuntimeDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("HandleMetadata.ProviderMismatch"); }));
		const FAssetDocumentVerifiedLeaseResult Source = Handles.AcquireVerifiedLease(Editor.GetValue(),
			FAssetDocumentHandleValidationContext{Transaction.GetMutationToken().RequestId, Transaction.GetMutationToken().TransactionId,
				Transaction.GetMutationToken().RuntimeRevision, TEXT("editor"), EAssetDocumentRuntimeRequestPhase::Apply,
				EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable});
		const FAssetDocumentVerifiedLeaseResult Target = Handles.AcquireVerifiedLease(AI.GetValue(), AIContext);
		if (!TestNotNull(TEXT("Editor source lease remains valid"), Source.Lease.Get())
			|| !TestNotNull(TEXT("AI target lease is valid"), Target.Lease.Get())) return false;
		Passed &= TestEqual(TEXT("target keeps request identity"), AI->RequestId, Editor->RequestId);
		Passed &= TestEqual(TEXT("target keeps transaction identity"), AI->TransactionId, Editor->TransactionId);
		Passed &= TestEqual(TEXT("target keeps runtime revision"), AI->RuntimeRevision, Editor->RuntimeRevision);
		Passed &= TestEqual(TEXT("target provider is AI"), AI->ProviderId, FString(TEXT("ai")));
		Passed &= TestTrue(TEXT("target preserves Asset/Mutable"), AI->Kind == EAssetDocumentHandleKind::Asset
			&& AI->Access == EAssetDocumentHandleAccess::Mutable);
		Passed &= TestNotEqual(TEXT("target occupies a fresh local slot"), AI->Slot, Editor->Slot);
		Passed &= TestTrue(TEXT("target preserves exact native payload identity"),
			&Source.Lease->GetPayloadChecked() == &Target.Lease->GetPayloadChecked());
	}

	// Property apply: shared action before footprint, one real authored write, and rollback.
	{
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeInvocation(KeyTypeApplyId, Handle.GetValue(), *Blackboard, EAssetDocumentRuntimeRequestPhase::Apply), Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("existing key property apply"), Outcome);
		Passed &= RequireExactChangedSet(*this, TEXT("property apply"), Outcome, KeyTypeApplyId, Blackboard->GetPathName(),
			FString::Printf(TEXT("asset:%s/Keys[name=ExistingKey].KeyType.Properties"), *Blackboard->GetPathName()));
		Passed &= TestTrue(TEXT("property apply returns the exact strict output DTO"), RuntimeValueEquals(Outcome.Value,
			ObjectValue({TEXT("canonical_properties"), TEXT("class_path")},
				{ObjectValue({TEXT("Int32Value")}, {IntegerValue(41)}),
					StringValue(UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass()->GetPathName())})));
		Passed &= TestEqual(TEXT("property apply action precedes footprint"), Transaction.Order,
			TArray<FString>{TEXT("action"), TEXT("footprint")});
		Passed &= TestEqual(TEXT("property apply emits one footprint"), Transaction.EnlistedFootprint.Num(), 1);
		if (Transaction.EnlistedFootprint.Num() == 1)
		{
			Passed &= TestEqual(TEXT("property apply footprint is exact"), Transaction.EnlistedFootprint[0].StableId,
				FString::Printf(TEXT("asset:%s/Keys[name=ExistingKey].KeyType.Properties"), *Blackboard->GetPathName()));
		}
		const UAssetDocumentAITypedPreimageTestLocalKeyType* AppliedKey =
			Blackboard->Keys.IsValidIndex(0)
				? Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[0].KeyType)
				: nullptr;
		Passed &= TestNotNull(TEXT("property apply preserves the admitted exact key type"), AppliedKey);
		if (AppliedKey != nullptr)
		{
			Passed &= TestEqual(TEXT("property apply writes exact value"), AppliedKey->Int32Value, 41);
		}
		Passed &= TestTrue(TEXT("property apply dirties package"), Blackboard->GetOutermost()->IsDirty());
		Passed &= TestTrue(TEXT("property apply rollback succeeds"), Transaction.Rollback(TEXT("Test.Rollback")).bValid);
		Passed &= TestTrue(TEXT("property apply rollback restores exact value and dirty state"), Before.Equals(*Blackboard));
	}

	// Rejection at either transaction boundary remains zero-forward-write; this is
	// actual fault injection, not repeated labels over the same terminal.
	for (const bool bRejectAction : {true, false})
	{
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		Transaction.RejectActionAtCall = bRejectAction ? 0 : INDEX_NONE;
		Transaction.RejectFootprintAtCall = bRejectAction ? INDEX_NONE : 0;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeInvocation(KeyTypeApplyId, Handle.GetValue(), *Blackboard, EAssetDocumentRuntimeRequestPhase::Apply), Handles, &Transaction);
		const FString Label = bRejectAction ? TEXT("property action rejection") : TEXT("property footprint rejection");
		Passed &= RequireNoForwardWrite(*this, Label, Outcome, Before, *Blackboard, Transaction);
		Passed &= TestTrue(*(Label + TEXT(" preserves injected diagnostic")),
			HasDiagnostic(Outcome, bRejectAction
				? TEXT("AITypedPreimage.RollbackActionEnlistmentFailed")
				: TEXT("Test.RejectFootprint")));
		Passed &= TestEqual(*(Label + TEXT(" has real ordering")), Transaction.Order,
			bRejectAction ? TArray<FString>{TEXT("action-rejected")} : TArray<FString>{TEXT("action"), TEXT("footprint-rejected")});
	}

	// Parent set, clear and cycle rejection cover the native derived projection and shared rollback.
	{
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Set = Provider->Invoke(
			MakeInvocation(ParentSetId, Handle.GetValue(), *Blackboard, EAssetDocumentRuntimeRequestPhase::Apply, Parent->GetPathName()), Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("parent set"), Set);
		Passed &= RequireExactChangedSet(*this, TEXT("parent set"), Set, ParentSetId, Blackboard->GetPathName(),
			FString::Printf(TEXT("asset:%s/Parent"), *Blackboard->GetPathName()));
		Passed &= TestEqual(TEXT("parent set output is strict lexicographic"), Set.Value.ObjectKeys,
			TArray<FString>{TEXT("inherited_key_names"), TEXT("local_key_names"), TEXT("parent_path_or_null")});
		Passed &= TestTrue(TEXT("parent set returns exact parent/inherited/local DTO"), RuntimeValueEquals(Set.Value,
			ObjectValue({TEXT("inherited_key_names"), TEXT("local_key_names"), TEXT("parent_path_or_null")},
				{ArrayValue({StringValue(TEXT("InheritedKey"))}),
					ArrayValue({StringValue(TEXT("ExistingKey")), StringValue(TEXT("SecondKey"))}),
					StringValue(Parent->GetPathName())})));
		Passed &= TestTrue(TEXT("parent set establishes exact native parent"), Blackboard->Parent == Parent);
		Passed &= TestTrue(TEXT("parent set has inherited derived projection"), Blackboard->GetNumKeys() > Blackboard->Keys.Num());
		Passed &= TestEqual(TEXT("parent set actions before footprint"), Transaction.Order,
			TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint")});
		Passed &= TestEqual(TEXT("parent set emits the complete two-item footprint"),
			Transaction.EnlistedFootprint.Num(), 2);
		if (Transaction.EnlistedFootprint.Num() == 2)
		{
			Passed &= TestEqual(TEXT("parent set Keys footprint path is first"),
				Transaction.EnlistedFootprint[0].StableId,
				FString::Printf(TEXT("asset:%s/Keys[*]"), *Blackboard->GetPathName()));
			Passed &= TestEqual(TEXT("parent set Keys footprint component is exact"),
				Transaction.EnlistedFootprint[0].RollbackComponentId,
				FString(TEXT("assetdocument.ai.blackboard.parent_keys.v1")));
			Passed &= TestEqual(TEXT("parent set Parent footprint path is second"),
				Transaction.EnlistedFootprint[1].StableId,
				FString::Printf(TEXT("asset:%s/Parent"), *Blackboard->GetPathName()));
			Passed &= TestEqual(TEXT("parent set Parent footprint component is exact"),
				Transaction.EnlistedFootprint[1].RollbackComponentId,
				FString(TEXT("assetdocument.ai.blackboard.parent.v1")));
		}
		Passed &= TestTrue(TEXT("parent set rollback succeeds"), Transaction.Rollback(TEXT("Test.Rollback")).bValid);
		Passed &= TestTrue(TEXT("parent set rollback records both shared actions"),
			Transaction.Order.Num() >= 2);
		if (Transaction.Order.Num() >= 2)
		{
			Passed &= TestEqual(TEXT("parent set rollback restores Parent first"),
				Transaction.Order[Transaction.Order.Num() - 2],
				FString(TEXT("rollback:assetdocument.ai.blackboard.parent.v1")));
			Passed &= TestEqual(TEXT("parent set rollback restores aggregate Keys second"),
				Transaction.Order[Transaction.Order.Num() - 1],
				FString(TEXT("rollback:assetdocument.ai.blackboard.parent_keys.v1")));
		}
		Passed &= RequireObservation(*this, TEXT("parent set rollback restores parent/projection/dirty"), Before, *Blackboard);
	}
	for (const int32 RejectedAction : {0, 1, 2})
	{
		SetFixtureParent(*Blackboard, nullptr, *Child);
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		Transaction.RejectActionAtCall = RejectedAction < 2 ? RejectedAction : INDEX_NONE;
		Transaction.RejectFootprintAtCall = RejectedAction == 2 ? 0 : INDEX_NONE;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeInvocation(ParentSetId, Handle.GetValue(), *Blackboard,
				EAssetDocumentRuntimeRequestPhase::Apply, Parent->GetPathName()), Handles, &Transaction);
		const FString Label = FString::Printf(TEXT("parent boundary rejection %d"), RejectedAction);
		Passed &= RequireNoForwardWrite(*this, Label, Outcome, Before, *Blackboard, Transaction);
		Passed &= TestTrue(*(Label + TEXT(" preserves exact injected diagnostic")),
			HasDiagnostic(Outcome, RejectedAction == 0
				? TEXT("AITypedPreimage.RollbackActionEnlistmentFailed")
				: RejectedAction == 1
					? TEXT("EditorNativeState.RollbackActionEnlistmentFailed")
					: TEXT("Test.RejectFootprint")));
		const TArray<FString> ExpectedOrder = RejectedAction == 0
			? TArray<FString>{TEXT("action-rejected")}
			: RejectedAction == 1
				? TArray<FString>{TEXT("action"), TEXT("action-rejected")}
				: TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint-rejected")};
		Passed &= TestEqual(*(Label + TEXT(" has exact ordering")), Transaction.Order, ExpectedOrder);
	}
	{
		SetFixtureParent(*Blackboard, Parent, *Child);
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Clear = Provider->Invoke(
			MakeInvocation(ParentSetId, Handle.GetValue(), *Blackboard, EAssetDocumentRuntimeRequestPhase::Apply), Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("parent clear"), Clear);
		Passed &= TestNull(TEXT("parent clear removes parent"), Blackboard->Parent);
		Passed &= TestTrue(TEXT("parent clear rollback succeeds"), Transaction.Rollback(TEXT("Test.Rollback")).bValid);
		Passed &= RequireObservation(*this, TEXT("parent clear rollback restores exact parent/dirty"), Before, *Blackboard);
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FBlackboardObservation Before(*Blackboard);
		const FAssetDocumentPrimitiveOutcome Cycle = Provider->Invoke(
			MakeInvocation(ParentSetId, Handle.GetValue(), *Blackboard, EAssetDocumentRuntimeRequestPhase::Apply, Child->GetPathName()), Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("parent cycle rejection"), Cycle, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("parent cycle has exact diagnostic"), HasDiagnostic(Cycle, TEXT("AIProvider.Blackboard.ParentCycle")));
	}

	// Existing local key reordering retains instance identity and metadata; all
	// non-admitted structural variants fail before a shared action/footprint.
	{
		SetFixtureParent(*Blackboard, Parent, *Child);
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		UBlackboardKeyType* ExistingInstance = Blackboard->Keys[0].KeyType;
		UBlackboardKeyType* SecondInstance = Blackboard->Keys[1].KeyType;
		TOptional<FBlackboardEntry> LocalSelfBefore;
		for (const FBlackboardEntry& Entry : Blackboard->Keys)
		{
			if (IsEngineDerivedSelfKey(Entry)) LocalSelfBefore = Entry;
		}
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Reorder = Provider->Invoke(MakeInvocation(LocalKeysReplaceId, Handle.GetValue(),
			*Blackboard, EAssetDocumentRuntimeRequestPhase::Apply, FString(), {ExistingKeyRow(TEXT("SecondKey"), 29), ExistingKeyRow(TEXT("ExistingKey"), 31)}), Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("local existing-key reorder"), Reorder);
		Passed &= RequireExactChangedSet(*this, TEXT("local reorder"), Reorder, LocalKeysReplaceId, Blackboard->GetPathName(),
			FString::Printf(TEXT("asset:%s/Keys[*]"), *Blackboard->GetPathName()));
		Passed &= TestEqual(TEXT("local reorder output keys are strict lexicographic"), Reorder.Value.ObjectKeys,
			TArray<FString>{TEXT("inherited_key_names"), TEXT("ordered_keys")});
		Passed &= TestTrue(TEXT("local reorder returns strict rows with canonical properties and name"), RuntimeValueEquals(Reorder.Value,
			ObjectValue({TEXT("inherited_key_names"), TEXT("ordered_keys")},
				{ArrayValue({StringValue(TEXT("InheritedKey"))}),
					ArrayValue({ExistingKeyRow(TEXT("SecondKey"), 29), ExistingKeyRow(TEXT("ExistingKey"), 31)})})));
		Passed &= TestEqual(TEXT("local reorder keeps requested first name"), Blackboard->Keys[0].EntryName, FName(TEXT("SecondKey")));
		Passed &= TestEqual(TEXT("local reorder keeps requested second name"), Blackboard->Keys[1].EntryName, FName(TEXT("ExistingKey")));
		Passed &= TestTrue(TEXT("local reorder preserves exact KeyType instances"), Blackboard->Keys[0].KeyType == SecondInstance
			&& Blackboard->Keys[1].KeyType == ExistingInstance);
		Passed &= TestEqual(TEXT("local reorder retains authored rows plus any pre-existing local SelfActor"),
			Blackboard->Keys.Num(), 2 + (LocalSelfBefore.IsSet() ? 1 : 0));
		if (LocalSelfBefore.IsSet() && Blackboard->Keys.Num() == 3)
		{
			const FBlackboardEntry& DerivedSelfAfter = Blackboard->Keys.Last();
			Passed &= TestTrue(TEXT("local reorder preserves exact native SelfActor KeyType identity"),
				DerivedSelfAfter.KeyType == LocalSelfBefore->KeyType);
			Passed &= TestEqual(TEXT("local reorder preserves native SelfActor name"),
				DerivedSelfAfter.EntryName, LocalSelfBefore->EntryName);
			Passed &= TestEqual(TEXT("local reorder preserves native SelfActor description"),
				DerivedSelfAfter.EntryDescription, LocalSelfBefore->EntryDescription);
			Passed &= TestEqual(TEXT("local reorder preserves native SelfActor category"),
				DerivedSelfAfter.EntryCategory, LocalSelfBefore->EntryCategory);
			Passed &= TestEqual(TEXT("local reorder preserves native SelfActor sync flag"),
				static_cast<bool>(DerivedSelfAfter.bInstanceSynced),
				static_cast<bool>(LocalSelfBefore->bInstanceSynced));
			Passed &= TestTrue(TEXT("local reorder keeps native SelfActor outside authored output"),
				IsEngineDerivedSelfKey(DerivedSelfAfter));
		}
		Passed &= TestTrue(TEXT("local reorder dirties package"), Blackboard->GetOutermost()->IsDirty());
		const UAssetDocumentAITypedPreimageTestLocalKeyType* ReorderedKey =
			Blackboard->Keys.IsValidIndex(0)
				? Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[0].KeyType)
				: nullptr;
		Passed &= TestNotNull(TEXT("local reorder preserves the admitted exact key type"), ReorderedKey);
		if (ReorderedKey != nullptr)
		{
			Passed &= TestEqual(TEXT("local reorder applies exact existing metadata value"), ReorderedKey->Int32Value, 29);
		}
		Passed &= TestEqual(TEXT("local reorder action before footprint"), Transaction.Order,
			TArray<FString>{TEXT("action"), TEXT("footprint")});
		Passed &= TestTrue(TEXT("local reorder rollback succeeds"), Transaction.Rollback(TEXT("Test.Rollback")).bValid);
		Passed &= RequireObservation(*this, TEXT("local reorder rollback restores order/properties/dirty"), Before, *Blackboard);
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FBlackboardObservation Before(*Blackboard);
		const FAssetDocumentPrimitiveOutcome Result = Provider->Invoke(MakeInvocation(LocalKeysReplaceId, Handle.GetValue(),
			*Blackboard, EAssetDocumentRuntimeRequestPhase::Apply, FString(), {ExistingKeyRow(TEXT("ExistingKey")), ExistingKeyRow(TEXT("ExistingKey"))}), Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("duplicate local key rejection"), Result, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("duplicate local key has exact diagnostic"), HasDiagnostic(Result, TEXT("AIProvider.Blackboard.DuplicateKey")));
	}
	{
		const FAssetDocumentRuntimeValueDto WrongClass = ObjectValue(
			{TEXT("canonical_properties"), TEXT("key_type_class_path"), TEXT("name")},
			{ObjectValue({TEXT("Int32Value")}, {IntegerValue(7)}),
				StringValue(UBlackboardKeyType_Bool::StaticClass()->GetPathName()), StringValue(TEXT("ExistingKey"))});
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FBlackboardObservation Before(*Blackboard);
		const FAssetDocumentPrimitiveOutcome Result = Provider->Invoke(MakeInvocation(LocalKeysReplaceId, Handle.GetValue(),
			*Blackboard, EAssetDocumentRuntimeRequestPhase::Apply, FString(), {WrongClass, ExistingKeyRow(TEXT("SecondKey"))}), Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("new key-type class rejection"), Result, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("new key-type class has blocked-authority diagnostic"),
			HasDiagnostic(Result, TEXT("AIProvider.Blackboard.NewKeyTypeRequiresBlockedAuthority")));
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FBlackboardObservation Before(*Blackboard);
		const FAssetDocumentPrimitiveOutcome Shadow = Provider->Invoke(MakeInvocation(LocalKeysReplaceId, Handle.GetValue(),
			*Blackboard, EAssetDocumentRuntimeRequestPhase::Apply, FString(), {ExistingKeyRow(TEXT("InheritedKey"))}), Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("inherited shadow rejection"), Shadow, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("inherited shadow has exact diagnostic"), HasDiagnostic(Shadow, TEXT("AIProvider.Blackboard.ShadowConflict")));
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FBlackboardObservation Before(*Blackboard);
		const FString SelfKeyName = FBlackboard::KeySelf.ToString();
		const FAssetDocumentPrimitiveOutcome Shadow = Provider->Invoke(MakeInvocation(
			LocalKeysReplaceId, Handle.GetValue(), *Blackboard,
			EAssetDocumentRuntimeRequestPhase::Apply, FString(),
			{ExistingKeyRow(*SelfKeyName)}), Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("authored SelfActor rejection"),
			Shadow, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("authored SelfActor has exact shadow diagnostic"),
			HasDiagnostic(Shadow, TEXT("AIProvider.Blackboard.ShadowConflict")));
	}
	{
		SetFixtureParent(*Blackboard, nullptr, *Child);
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		if (!TestTrue(TEXT("empty authored-list fixture has one exact local SelfActor"),
			Blackboard->Keys.Num() == 3 && IsEngineDerivedSelfKey(Blackboard->Keys.Last()))) return false;
		const FBlackboardEntry LocalSelfBefore = Blackboard->Keys.Last();
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Empty = Provider->Invoke(MakeInvocation(LocalKeysReplaceId, Handle.GetValue(),
			*Blackboard, EAssetDocumentRuntimeRequestPhase::Apply, FString(), {}), Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("empty local-key list is valid"), Empty);
		Passed &= TestTrue(TEXT("empty local-key list preserves only engine-derived SelfActor"),
			Blackboard->Keys.Num() == 1 && IsEngineDerivedSelfKey(Blackboard->Keys[0]));
		if (Blackboard->Keys.Num() == 1)
		{
			const FBlackboardEntry& LocalSelfAfter = Blackboard->Keys[0];
			Passed &= TestTrue(TEXT("empty authored list preserves exact SelfActor KeyType identity"),
				LocalSelfAfter.KeyType == LocalSelfBefore.KeyType);
			Passed &= TestEqual(TEXT("empty authored list preserves exact SelfActor description"),
				LocalSelfAfter.EntryDescription, LocalSelfBefore.EntryDescription);
			Passed &= TestEqual(TEXT("empty authored list preserves exact SelfActor category"),
				LocalSelfAfter.EntryCategory, LocalSelfBefore.EntryCategory);
			Passed &= TestEqual(TEXT("empty authored list preserves exact SelfActor sync flag"),
				static_cast<bool>(LocalSelfAfter.bInstanceSynced),
				static_cast<bool>(LocalSelfBefore.bInstanceSynced));
		}
		Passed &= TestTrue(TEXT("empty local-key list dirties package"), Blackboard->GetOutermost()->IsDirty());
		Passed &= TestTrue(TEXT("empty local-key rollback succeeds"), Transaction.Rollback(TEXT("Test.Rollback")).bValid);
		Passed &= RequireObservation(*this, TEXT("empty local-key rollback restores order/properties/dirty"), Before, *Blackboard);
	}

	// The production-owned, test-only seam is deliberately placed after the
	// authoritative forward boundary.  The failure outcome is empty, while the
	// retained shared action remains responsible for explicit exact rollback.
	{
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint FailurePoint(
				EAssetDocumentAIBlackboardTestFailurePoint::AfterKeyTypeAuthoredAssignment);
			Outcome = Provider->Invoke(MakeInvocation(KeyTypeApplyId, Handle.GetValue(), *Blackboard,
				EAssetDocumentRuntimeRequestPhase::Apply), Handles, &Transaction);
		}
		Passed &= TestEqual(TEXT("post-key-type authored seam fails"), Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		Passed &= TestTrue(TEXT("post-key-type authored seam uses exact rejected terminal"),
			HasDiagnostic(Outcome, TEXT("AIProvider.OperationRejected")));
		const UAssetDocumentAITypedPreimageTestLocalKeyType* ForwardKey =
			Blackboard->Keys.IsValidIndex(0)
				? Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[0].KeyType)
				: nullptr;
		Passed &= TestNotNull(TEXT("post-key-type seam preserves the admitted exact key type"), ForwardKey);
		if (ForwardKey != nullptr)
		{
			Passed &= TestEqual(TEXT("post-key-type seam performs the forward authored write"), ForwardKey->Int32Value, 41);
		}
		Passed &= TestEqual(TEXT("post-key-type seam retains action-before-footprint"), Transaction.Order,
			TArray<FString>{TEXT("action"), TEXT("footprint")});
		Passed &= TestTrue(TEXT("post-key-type explicit shared-action rollback succeeds"),
			Transaction.Rollback(TEXT("Test.AfterKeyTypeAuthoredAssignment")).bValid);
		Passed &= RequireObservation(*this, TEXT("post-key-type rollback restores authored/derived/dirty preimage"), Before, *Blackboard);
	}
	{
		SetFixtureParent(*Blackboard, nullptr, *Child);
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint FailurePoint(
				EAssetDocumentAIBlackboardTestFailurePoint::AfterParentNativeRefresh);
			Outcome = Provider->Invoke(MakeInvocation(ParentSetId, Handle.GetValue(), *Blackboard,
				EAssetDocumentRuntimeRequestPhase::Apply, Parent->GetPathName()), Handles, &Transaction);
		}
		Passed &= TestEqual(TEXT("post-parent refresh seam fails"), Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		Passed &= TestTrue(TEXT("post-parent refresh seam uses exact rejected terminal"),
			HasDiagnostic(Outcome, TEXT("AIProvider.OperationRejected")));
		Passed &= TestTrue(TEXT("post-parent refresh seam reached native parent/derived projection"),
			Blackboard->Parent == Parent && Blackboard->GetNumKeys() > Blackboard->Keys.Num());
		Passed &= TestTrue(TEXT("post-parent refresh explicit shared-action rollback succeeds"),
			Transaction.Rollback(TEXT("Test.AfterParentNativeRefresh")).bValid);
		Passed &= RequireObservation(*this, TEXT("post-parent refresh rollback restores authored/derived/dirty preimage"), Before, *Blackboard);
	}
	{
		SetFixtureParent(*Blackboard, Parent, *Child);
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint FailurePoint(
				EAssetDocumentAIBlackboardTestFailurePoint::AfterLocalKeyIdRefresh);
			Outcome = Provider->Invoke(MakeInvocation(LocalKeysReplaceId, Handle.GetValue(), *Blackboard,
				EAssetDocumentRuntimeRequestPhase::Apply, FString(),
				{ExistingKeyRow(TEXT("SecondKey"), 29), ExistingKeyRow(TEXT("ExistingKey"), 31)}), Handles, &Transaction);
		}
		Passed &= TestEqual(TEXT("post-local-key-id refresh seam fails"), Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		Passed &= TestTrue(TEXT("post-local-key-id refresh seam uses exact rejected terminal"),
			HasDiagnostic(Outcome, TEXT("AIProvider.OperationRejected")));
		Passed &= TestEqual(TEXT("post-local-key-id seam reached requested order"), Blackboard->Keys[0].EntryName,
			FName(TEXT("SecondKey")));
		Passed &= TestTrue(TEXT("post-local-key-id explicit shared-action rollback succeeds"),
			Transaction.Rollback(TEXT("Test.AfterLocalKeyIdRefresh")).bValid);
		Passed &= RequireObservation(*this, TEXT("post-local-key-id rollback restores authored/derived/dirty preimage"), Before, *Blackboard);
	}

	// Strict DTO admission rejects syntactically non-canonical object/class paths
	// before target resolution, capture, footprint, or forward write.
	for (const int32 InvalidCase : {0, 1, 2})
	{
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveInvocation Invalid = InvalidCase == 0
			? MakeInvocation(KeyTypeApplyId, Handle.GetValue(), *Blackboard,
				EAssetDocumentRuntimeRequestPhase::Apply)
			: InvalidCase == 1
				? MakeInvocation(ParentSetId, Handle.GetValue(), *Blackboard,
					EAssetDocumentRuntimeRequestPhase::Apply, TEXT("not-a-canonical-object-path"))
				: MakeInvocation(LocalKeysReplaceId, Handle.GetValue(), *Blackboard,
					EAssetDocumentRuntimeRequestPhase::Apply, FString(),
					{ExistingKeyRow(TEXT("ExistingKey"))});
		if (InvalidCase == 0)
		{
			Invalid.ValueArguments.Elements[0].StringValue = TEXT("not-a-canonical-object-path");
		}
		else if (InvalidCase == 2)
		{
			Invalid.ValueArguments.Elements[1].Elements[0].Elements[1].StringValue =
				TEXT("not-a-canonical-class-path");
		}
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Invalid, Handles, &Transaction);
		const FString Label = FString::Printf(TEXT("non-canonical path case %d"), InvalidCase);
		Passed &= RequireNoForwardWrite(*this, Label, Outcome, Before, *Blackboard, Transaction);
		Passed &= TestTrue(*(Label + TEXT(" uses strict shape terminal")),
			HasDiagnostic(Outcome, TEXT("AIProvider.InvalidValueShape")));
	}

	// A loaded non-Blackboard object is distinct from an unavailable path.
	{
		UObject* NonBlackboardParent = NewObject<UBlackboardKeyType_Bool>(GetTransientPackage());
		if (!TestNotNull(TEXT("loaded non-Blackboard parent fixture exists"), NonBlackboardParent)) return false;
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(MakeInvocation(ParentSetId, Handle.GetValue(),
			*Blackboard, EAssetDocumentRuntimeRequestPhase::Apply, NonBlackboardParent->GetPathName()), Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("loaded parent class mismatch"), Outcome, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("loaded parent class mismatch has exact diagnostic"),
			HasDiagnostic(Outcome, TEXT("AIProvider.Blackboard.ParentClassMismatch")));
	}

	// Revision drift cannot become a write: the invalid transaction is rejected
	// before handler dispatch, and an otherwise live transaction rejects a stale
	// target-handle revision at the public handle metadata boundary.
	{
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveInvocation TransactionMismatch = MakeInvocation(KeyTypeApplyId, Handle.GetValue(),
			*Blackboard, EAssetDocumentRuntimeRequestPhase::Apply);
		++TransactionMismatch.RuntimeRevision;
		const FAssetDocumentPrimitiveOutcome TransactionOutcome = Provider->Invoke(TransactionMismatch, Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("runtime revision transaction mismatch"), TransactionOutcome,
			Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("runtime revision transaction mismatch has exact terminal"),
			HasDiagnostic(TransactionOutcome, TEXT("AIProvider.InvalidTransaction")));

		FAssetDocumentOpaqueHandle StaleHandle = Handle.GetValue();
		++StaleHandle.RuntimeRevision;
		FAssetDocumentPrimitiveInvocation HandleMismatch = MakeInvocation(KeyTypeApplyId, Handle.GetValue(),
			*Blackboard, EAssetDocumentRuntimeRequestPhase::Apply);
		HandleMismatch.HandleArguments[0].Handle = StaleHandle;
		const FAssetDocumentPrimitiveOutcome HandleOutcome = Provider->Invoke(
			HandleMismatch, Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("runtime revision handle mismatch"), HandleOutcome,
			Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("runtime revision handle mismatch preserves exact metadata terminal"),
			HasDiagnostic(HandleOutcome, TEXT("HandleMetadata.RuntimeRevisionMismatch")));
	}

	// This resolver's explicit target-provider invalidation models only the
	// provider-local lease lifetime after the transaction terminal.  It does not
	// claim Runtime orchestration; PackageUnload/PackageReload coverage remains
	// owned by AssetDocumentRequestCoordinatorContractTests and
	// AssetDocumentHandleRegistryContractTests, because a transient fixture cannot
	// prove real package unload/reload without global residue.
	for (const EAssetDocumentHandleInvalidationReason Reason : {
		EAssetDocumentHandleInvalidationReason::Commit, EAssetDocumentHandleInvalidationReason::Rollback})
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = MakeAIHandle(Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentHandleValidationContext AIContext;
		AIContext.RequestId = Transaction.GetMutationToken().RequestId;
		AIContext.TransactionId = Transaction.GetMutationToken().TransactionId;
		AIContext.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
		AIContext.ProviderId = TEXT("ai");
		AIContext.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
		AIContext.RequiredKind = EAssetDocumentHandleKind::Asset;
		AIContext.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
		Passed &= TestNotNull(TEXT("pre-terminal provider-local target lease is live"),
			Handles.AcquireVerifiedLease(Handle.GetValue(), AIContext).Lease.Get());
		FAssetDocumentHandleInvalidation Event;
		Event.Reason = Reason;
		Event.RequestId = Transaction.GetMutationToken().RequestId;
		Event.TransactionId = Transaction.GetMutationToken().TransactionId;
		Event.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
		Event.ProviderId = TEXT("ai");
		const FAssetDocumentHandleInvalidationResult Invalidated = Handles.Invalidate(Event);
		Passed &= TestEqual(TEXT("transaction terminal invalidates exactly the provider-local target"),
			Invalidated.InvalidatedHandleCount, 1u);
		const FAssetDocumentVerifiedLeaseResult AfterTerminal = Handles.AcquireVerifiedLease(Handle.GetValue(), AIContext);
		Passed &= TestNull(TEXT("transaction terminal leaves no provider-local target lease"), AfterTerminal.Lease.Get());
	}

	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReorderAdmissionTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Reorder.Admission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReorderAdmissionTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	FBlackboardLifecycleHandleResolver Handles;
	FAssetDocumentPrimitiveInvocation Invalid;
	Invalid.PrimitiveId = LocalKeysReorderV2Id;
	Invalid.Version = 2;
	Invalid.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invalid.ValueArguments = ObjectValue({TEXT("blackboard_path")},
		{StringValue(TEXT("/Game/AssetDocumentTests/Reorder.Reorder"))});
	const FAssetDocumentPrimitiveOutcome InvalidOutcome =
		HandleAssetDocumentAIBlackboardLocalKeysReorder(Invalid, Handles, nullptr);
	bool Passed = TestEqual(TEXT("SC-REORDER malformed admission request fails"),
		InvalidOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("SC-REORDER malformed admission request has exact terminal"),
		InvalidOutcome.Diagnostics.Num() == 1
			&& HasDiagnostic(InvalidOutcome, TEXT("AIProvider.InvalidValueShape")));
	Passed &= TestTrue(TEXT("SC-REORDER malformed admission request has zero residue"),
		InvalidOutcome.Value.Elements.IsEmpty() && InvalidOutcome.Value.ObjectKeys.IsEmpty()
			&& InvalidOutcome.HandleResults.IsEmpty() && InvalidOutcome.ChangedSet.IsEmpty());

	FAssetDocumentPrimitiveInvocation Valid = Invalid;
	Valid.ValueArguments = ObjectValue({TEXT("blackboard_path"), TEXT("ordered_local_key_names")},
		{StringValue(TEXT("/Game/AssetDocumentTests/Reorder.Reorder")),
			ArrayValue({StringValue(TEXT("First")), StringValue(TEXT("Second"))})});
	const FAssetDocumentPrimitiveOutcome ValidOutcome =
		HandleAssetDocumentAIBlackboardLocalKeysReorder(Valid, Handles, nullptr);
	Passed &= TestEqual(TEXT("SC-REORDER valid envelope without its active transaction fails"),
		ValidOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("SC-REORDER valid admission request has exact terminal"),
		ValidOutcome.Diagnostics.Num() == 1
			&& HasDiagnostic(ValidOutcome, TEXT("AIProvider.InvalidTransaction")));
	Passed &= TestTrue(TEXT("SC-REORDER valid admission request has zero residue"),
		ValidOutcome.Value.Elements.IsEmpty() && ValidOutcome.Value.ObjectKeys.IsEmpty()
			&& ValidOutcome.HandleResults.IsEmpty() && ValidOutcome.ChangedSet.IsEmpty());
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReorderSuccessTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Reorder.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReorderSuccessTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: an owned-local identity request is a no-op, while a
	//   valid permutation atomically reorders the complete authored projection.
	// - Candidate technique: state-transition testing across identity -> fresh
	//   reorder -> rollback, including derived Key-ID and loaded-consumer caches.
	// - Risk or gap: a superficial array permutation can lose canonical Struct
	//   DTO data, move engine-derived SelfActor, or leave derived consumers stale.
	// - Test: RetainedAdmittedStructCanonical (SC-ORD-001).
	// - Expected observable outcome: literal DTOs, stable same-name metadata and
	//   KeyType instances, one order-only ChangedSet on permutation, exact rollback.
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ReorderStruct_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* HardObject = NewObject<UBlackboardData>(
		Package, TEXT("HardObject"), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("SC-ORD-001 Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("SC-ORD-001 child fixture exists"), Child)
		|| !TestNotNull(TEXT("SC-ORD-001 hard reference fixture exists"), HardObject))
	{
		return false;
	}
	TStrongObjectPtr<UBlackboardData> BlackboardGuard(Blackboard);
	TStrongObjectPtr<UBlackboardData> ChildGuard(Child);
	TStrongObjectPtr<UBlackboardData> HardObjectGuard(HardObject);
	// PostInitProperties installs persistent engine-derived keys before this
	// fixture authors its local rows. Preserve those exact entries, but append
	// them after the authored projection so the physical order matches the
	// publication contract exercised below.
	TArray<FBlackboardEntry> DerivedKeys = MoveTemp(Blackboard->Keys);
	Blackboard->Keys.Reset();
	AddExistingKey(*Blackboard, TEXT("First"), 1);
	UBlackboardKeyType_Struct* StructKeyType = NewObject<UBlackboardKeyType_Struct>(
		Blackboard, TEXT("RetainedStructKeyType"), RF_Transactional);
	FProperty* DefaultValueProperty = FindFProperty<FProperty>(
		UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	if (!TestNotNull(TEXT("SC-ORD-001 retained Struct KeyType exists"), StructKeyType)
		|| !TestNotNull(TEXT("SC-ORD-001 Struct DefaultValue property exists"),
			DefaultValueProperty)) return false;
	// This is the exact editable lifecycle required for the real Struct fixture;
	// the canonical DTO below remains handwritten test authority.
	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	auto& Stored = StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	Stored.ArrayValues = {9, -2};
	Stored.BoolValue = true;
	Stored.ByteValue = 255;
	Stored.ClassValue = UObject::StaticClass();
	Stored.DoubleValue = 12.5;
	Stored.DynamicValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Stored.DynamicValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Count = 4;
	Stored.DynamicValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Label = TEXT("dynamic");
	Stored.EnumValue = EAssetDocumentAIBlackboardCanonicalStructTestEnum::Second;
	Stored.FloatValue = 1.25f;
	Stored.HardObject = HardObject;
	Stored.Int64Value = MIN_int64 + 7;
	Stored.IntValue = -17;
	Stored.MapValues.Add(TEXT("b"), 2);
	Stored.MapValues.Add(TEXT("a"), 1);
	Stored.NameValue = FName(TEXT("CanonicalName_7"));
	Stored.NestedValue.Count = 3;
	Stored.NestedValue.Label = TEXT("nested");
	Stored.SetValues.Add(TEXT("zulu"));
	Stored.SetValues.Add(TEXT("alpha"));
	Stored.SoftClassValue = TSoftClassPtr<UObject>(UObject::StaticClass());
	Stored.SoftObjectValue = TSoftObjectPtr<UObject>(HardObject);
	Stored.StaticValues[0] = 5;
	Stored.StaticValues[1] = 6;
	Stored.StaticValues[2] = 7;
	Stored.StringValue = TEXT("canonical");
	Stored.TextValue = FText::AsCultureInvariant(TEXT("literal-equivalent"));
	Stored.UInt16Value = MAX_uint16;
	Stored.UInt32Value = MAX_uint32;
	Stored.UInt64Value = MAX_uint64;
	FPropertyChangedEvent DefaultValueChanged(
		DefaultValueProperty, EPropertyChangeType::ValueSet);
	DefaultValueChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(DefaultValueChanged);
	FBlackboardEntry StructEntry;
	StructEntry.EntryName = TEXT("RetainedStruct");
	StructEntry.EntryDescription = TEXT("Retained canonical Struct");
	StructEntry.EntryCategory = TEXT("Retention");
	StructEntry.KeyType = StructKeyType;
	StructEntry.bInstanceSynced = true;
	Blackboard->Keys.Add(MoveTemp(StructEntry));
	// FValueOrBBKey_Int32 admits only the exact stock Int KeyType. Keep Last as
	// that real type so the loaded-consumer cache is valid before Reorder runs.
	AddExistingStockIntKey(*Blackboard, TEXT("Last"), 3);
	Blackboard->Keys.Append(MoveTemp(DerivedKeys));
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Package->SetDirtyFlag(false);
	if (!TestTrue(TEXT("SC-ORD-001 has physical engine-derived SelfActor tail"),
		Blackboard->Keys.Num() == 4 && IsEngineDerivedSelfKey(Blackboard->Keys.Last()))) return false;

	const auto StructValue = [](const UScriptStruct& Struct, TArray<FString> Keys,
		TArray<FAssetDocumentRuntimeValueDto> Values)
	{
		return ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue(MoveTemp(Keys), MoveTemp(Values)), StringValue(Struct.GetPathName())});
	};
	const FAssetDocumentRuntimeValueDto Nested = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(3), StringValue(TEXT("nested"))});
	const FAssetDocumentRuntimeValueDto Dynamic = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(4), StringValue(TEXT("dynamic"))});
	const FAssetDocumentRuntimeValueDto CanonicalStruct = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct(),
		{TEXT("ArrayValues"), TEXT("BoolValue"), TEXT("ByteValue"), TEXT("ClassValue"),
			TEXT("DoubleValue"), TEXT("DynamicValue"), TEXT("EnumValue"), TEXT("FloatValue"),
			TEXT("HardObject"), TEXT("Int64Value"), TEXT("IntValue"), TEXT("MapValues"),
			TEXT("NameValue"), TEXT("NestedValue"), TEXT("SetValues"), TEXT("SoftClassValue"),
			TEXT("SoftObjectValue"), TEXT("StaticValues"), TEXT("StringValue"), TEXT("TextValue"),
			TEXT("UInt16Value"), TEXT("UInt32Value"), TEXT("UInt64Value")},
		{ArrayValue({IntegerValue(9), IntegerValue(-2)}), BooleanValue(true), IntegerValue(255),
			ReferenceValue(TEXT("ClassRef"), *UObject::StaticClass()), NumberValue(12.5), Dynamic,
			ObjectValue({TEXT("Enum"), TEXT("Enumerator")},
				{StringValue(StaticEnum<EAssetDocumentAIBlackboardCanonicalStructTestEnum>()->GetPathName()),
					StringValue(TEXT("Second"))}),
			NumberValue(1.25), ReferenceValue(TEXT("AssetRef"), *HardObject),
			IntegerValue(MIN_int64 + 7), IntegerValue(-17),
			ArrayValue({ObjectValue({TEXT("Key"), TEXT("Value")},
				{StringValue(TEXT("a")), IntegerValue(1)}),
				ObjectValue({TEXT("Key"), TEXT("Value")},
				{StringValue(TEXT("b")), IntegerValue(2)})}),
			StringValue(TEXT("CanonicalName_7")), Nested,
			ArrayValue({StringValue(TEXT("zulu")), StringValue(TEXT("alpha"))}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftClassRef")), StringValue(UObject::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftAssetRef")), StringValue(HardObject->GetPathName())}),
			ArrayValue({IntegerValue(5), IntegerValue(6), IntegerValue(7)}),
			StringValue(TEXT("canonical")), StringValue(TEXT("literal-equivalent")),
			IntegerValue(MAX_uint16), IntegerValue(MAX_uint32),
			ObjectValue({TEXT("Kind"), TEXT("Value")},
				{StringValue(TEXT("UInt64")), StringValue(TEXT("18446744073709551615"))})});
	const FAssetDocumentRuntimeValueDto ExpectedStructEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("Retention")), StringValue(TEXT("Retained canonical Struct")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {CanonicalStruct}),
			StringValue(TEXT("RetainedStruct")), BooleanValue(true)});
	const FAssetDocumentRuntimeValueDto FirstEntry =
		MakeEntryReplaceClassV2OriginalEntry(TEXT("First"), 1);
	const FAssetDocumentRuntimeValueDto LastEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("")), StringValue(TEXT("")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Int::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {IntegerValue(3)}),
			StringValue(TEXT("Last")), BooleanValue(false)});
	const FAssetDocumentRuntimeValueDto IdentityOutput = ObjectValue(
		{TEXT("entries"), TEXT("ordered_local_key_names")},
		{ArrayValue({FirstEntry, ExpectedStructEntry, LastEntry}),
			ArrayValue({StringValue(TEXT("First")), StringValue(TEXT("RetainedStruct")),
				StringValue(TEXT("Last"))})});
	const FAssetDocumentRuntimeValueDto ReorderedOutput = ObjectValue(
		{TEXT("entries"), TEXT("ordered_local_key_names")},
		{ArrayValue({ExpectedStructEntry, LastEntry, FirstEntry}),
			ArrayValue({StringValue(TEXT("RetainedStruct")), StringValue(TEXT("Last")),
				StringValue(TEXT("First"))})});

	const auto FindLocalEntry = [](const UBlackboardData& Owner, const FName Name)
	{
		return Owner.Keys.FindByPredicate([Name](const FBlackboardEntry& Candidate)
		{
			return Candidate.EntryName == Name;
		});
	};
	const FBlackboardEntry* FirstBeforeEntry = FindLocalEntry(*Blackboard, TEXT("First"));
	const FBlackboardEntry* StructBeforeEntry = FindLocalEntry(*Blackboard, TEXT("RetainedStruct"));
	const FBlackboardEntry* LastBeforeEntry = FindLocalEntry(*Blackboard, TEXT("Last"));
	if (!TestNotNull(TEXT("SC-ORD-001 First entry exists"), FirstBeforeEntry)
		|| !TestNotNull(TEXT("SC-ORD-001 retained Struct entry exists"), StructBeforeEntry)
		|| !TestNotNull(TEXT("SC-ORD-001 Last entry exists"), LastBeforeEntry)) return false;
	const FBlackboardEntry FirstBefore = *FirstBeforeEntry;
	const FBlackboardEntry StructBefore = *StructBeforeEntry;
	const FBlackboardEntry LastBefore = *LastBeforeEntry;
	const FBlackboardEntry SelfBefore = Blackboard->Keys.Last();
	const FBlackboard::FKey FirstBeforeId = Blackboard->GetKeyID(TEXT("First"));
	const FBlackboard::FKey StructBeforeId = Blackboard->GetKeyID(TEXT("RetainedStruct"));
	const FBlackboard::FKey LastBeforeId = Blackboard->GetKeyID(TEXT("Last"));
	const FBlackboard::FKey SelfBeforeId = Blackboard->GetKeyID(FBlackboard::KeySelf);
	TUniquePtr<FRemoveLoadedConsumerFixture> Consumer = MakeRemoveLoadedConsumerFixture(
		*this, *Blackboard, TEXT("First"), TEXT("Last"));
	if (!Consumer.IsValid()
		|| !PrimeRemoveLoadedConsumer(*this, *Consumer, *Blackboard,
			TEXT("First"), TEXT("Last"), 211)) return false;
	const bool bBeforeConsumerTreeDirty = Consumer->Tree->GetOutermost()->IsDirty();
	const FBlackboardObservation Before(*Blackboard);
	const FBlackboardObservation ChildBefore(*Child);
	const auto RequireSameEntryMetadata = [&](const FString& Label,
		const FName Name, const FBlackboardEntry& Expected)
	{
		const FBlackboardEntry* Actual = FindLocalEntry(*Blackboard, Name);
		return TestNotNull(*(Label + TEXT(" entry exists")), Actual)
			&& TestEqual(*(Label + TEXT(" preserves KeyType pointer")),
				Actual != nullptr ? Actual->KeyType.Get() : nullptr, Expected.KeyType.Get())
			&& TestEqual(*(Label + TEXT(" preserves Description")),
				Actual != nullptr ? Actual->EntryDescription : FString(), Expected.EntryDescription)
			&& TestEqual(*(Label + TEXT(" preserves Category")),
				Actual != nullptr ? Actual->EntryCategory : FName(), Expected.EntryCategory)
			&& TestEqual(*(Label + TEXT(" preserves synchronized metadata")),
				Actual != nullptr ? static_cast<bool>(Actual->bInstanceSynced) : false,
				static_cast<bool>(Expected.bInstanceSynced));
	};

	bool Passed = true;
	// A. Identity is complete readback success, but has no publication or transaction residue.
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeLocalKeysReorderV2Invocation(Handle.GetValue(), *Blackboard,
				{StringValue(TEXT("First")), StringValue(TEXT("RetainedStruct")),
					StringValue(TEXT("Last"))}), Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("SC-ORD-001 identity authored order"), Outcome);
		if (Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		{
			Passed &= TestTrue(TEXT("SC-ORD-001 identity returns literal complete DTO/order"),
				RuntimeValueEquals(Outcome.Value, IdentityOutput));
			Passed &= TestTrue(TEXT("SC-ORD-001 identity returns no handles or ChangedSet"),
				Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
			Passed &= TestEqual(TEXT("SC-ORD-001 identity enlists zero actions"),
				Transaction.ActionEnlistCount, 0);
			Passed &= TestEqual(TEXT("SC-ORD-001 identity enlists zero footprints"),
				Transaction.FootprintEnlistCount, 0);
			Passed &= TestTrue(TEXT("SC-ORD-001 identity has zero transaction ordering"),
				Transaction.Order.IsEmpty());
			Passed &= RequireObservation(*this, TEXT("SC-ORD-001 identity preserves target"),
				Before, *Blackboard);
			Passed &= RequireObservation(*this, TEXT("SC-ORD-001 identity preserves child"),
				ChildBefore, *Child);
			Passed &= TestEqual(TEXT("SC-ORD-001 identity preserves authored Key IDs"),
				TArray<FBlackboard::FKey>{Blackboard->GetKeyID(TEXT("First")),
					Blackboard->GetKeyID(TEXT("RetainedStruct")), Blackboard->GetKeyID(TEXT("Last"))},
				TArray<FBlackboard::FKey>{FirstBeforeId, StructBeforeId, LastBeforeId});
			Passed &= TestEqual(TEXT("SC-ORD-001 identity preserves SelfActor Key-ID"),
				Blackboard->GetKeyID(FBlackboard::KeySelf), SelfBeforeId);
			Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 identity First"), TEXT("First"), FirstBefore);
			Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 identity Struct"), TEXT("RetainedStruct"), StructBefore);
			Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 identity Last"), TEXT("Last"), LastBefore);
			Passed &= TestEqual(TEXT("SC-ORD-001 identity preserves loaded selector cache"),
				Consumer->Task->DirectSelector.GetSelectedKeyID(), FirstBeforeId);
			TStrongObjectPtr<UBlackboardComponent> Component =
				MakeRemoveConsumerComponent(*this, *Blackboard);
			if (!Component.IsValid()) return false;
			Component->SetValueAsInt(TEXT("Last"), 211);
			Passed &= TestEqual(TEXT("SC-ORD-001 identity preserves ValueOr cache semantics"),
				Consumer->Task->ValueOrInt32.GetValue(Component.Get()), 211);
		}
	}

	// B. Use a fresh transaction for a real three-key permutation.
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeLocalKeysReorderV2Invocation(Handle.GetValue(), *Blackboard,
				{StringValue(TEXT("RetainedStruct")), StringValue(TEXT("Last")),
					StringValue(TEXT("First"))}), Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("SC-ORD-001 fresh real permutation"), Outcome);
		if (Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		{
			Passed &= TestTrue(TEXT("SC-ORD-001 permutation returns literal complete DTO/order"),
				RuntimeValueEquals(Outcome.Value, ReorderedOutput));
			Passed &= TestTrue(TEXT("SC-ORD-001 permutation returns no handles"),
				Outcome.HandleResults.IsEmpty());
			Passed &= RequireExactChangedSet(*this, TEXT("SC-ORD-001 permutation"), Outcome,
				LocalKeysReorderV2Id, Blackboard->GetPathName(),
				FString::Printf(TEXT("asset:%s/Keys.@Order"), *Blackboard->GetPathName()));
			Passed &= TestEqual(TEXT("SC-ORD-001 permutation captures one action then footprint"),
				Transaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")});
			Passed &= TestEqual(TEXT("SC-ORD-001 permutation enlists exactly one footprint"),
				Transaction.EnlistedFootprint.Num(), 1);
			Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 permutation First"), TEXT("First"), FirstBefore);
			Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 permutation Struct"), TEXT("RetainedStruct"), StructBefore);
			Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 permutation Last"), TEXT("Last"), LastBefore);
			Passed &= TestTrue(TEXT("SC-ORD-001 permutation leaves SelfActor physical tail"),
				Blackboard->Keys.Num() == 4 && IsEngineDerivedSelfKey(Blackboard->Keys.Last())
					&& Blackboard->Keys.Last().KeyType == SelfBefore.KeyType);
			Passed &= TestEqual(TEXT("SC-ORD-001 permutation stores only authored request plus SelfActor"),
				TArray<FName>{Blackboard->Keys[0].EntryName, Blackboard->Keys[1].EntryName,
					Blackboard->Keys[2].EntryName, Blackboard->Keys[3].EntryName},
				TArray<FName>{TEXT("RetainedStruct"), TEXT("Last"), TEXT("First"), FBlackboard::KeySelf});
			Passed &= TestNotEqual(TEXT("SC-ORD-001 permutation moves First Key-ID"),
				Blackboard->GetKeyID(TEXT("First")), FirstBeforeId);
			Passed &= TestNotEqual(TEXT("SC-ORD-001 permutation moves Struct Key-ID"),
				Blackboard->GetKeyID(TEXT("RetainedStruct")), StructBeforeId);
			Passed &= TestNotEqual(TEXT("SC-ORD-001 permutation moves Last Key-ID"),
				Blackboard->GetKeyID(TEXT("Last")), LastBeforeId);
			Passed &= TestEqual(TEXT("SC-ORD-001 permutation preserves SelfActor Key-ID"),
				Blackboard->GetKeyID(FBlackboard::KeySelf), SelfBeforeId);
			for (const FName Name : {FName(TEXT("First")), FName(TEXT("RetainedStruct")), FName(TEXT("Last"))})
			{
				const FBlackboard::FKey TargetId = Blackboard->GetKeyID(Name);
				const FBlackboard::FKey ChildId = Child->GetKeyID(Name);
				Passed &= TestEqual(*(FString(TEXT("SC-ORD-001 permutation target resolves same name ")) + Name.ToString()),
					Blackboard->GetKeyName(TargetId), Name);
				Passed &= TestEqual(*(FString(TEXT("SC-ORD-001 permutation child resolves same name ")) + Name.ToString()),
					Child->GetKeyName(ChildId), Name);
				Passed &= TestEqual(*(FString(TEXT("SC-ORD-001 permutation child preserves type ")) + Name.ToString()),
					Child->GetKeyType(ChildId).Get(), Blackboard->GetKeyType(TargetId).Get());
			}
			Passed &= TestEqual(TEXT("SC-ORD-001 permutation refreshes loaded selector Key-ID"),
				Consumer->Task->DirectSelector.GetSelectedKeyID(), Blackboard->GetKeyID(TEXT("First")));
			TStrongObjectPtr<UBlackboardComponent> Component =
				MakeRemoveConsumerComponent(*this, *Blackboard);
			if (!Component.IsValid()) return false;
			Component->SetValueAsInt(TEXT("Last"), 311);
			Passed &= TestEqual(TEXT("SC-ORD-001 permutation preserves ValueOr cache semantics"),
				Consumer->Task->ValueOrInt32.GetValue(Component.Get()), 311);
			Passed &= TestTrue(TEXT("SC-ORD-001 permutation dirties published package"), Package->IsDirty());
		}
		const FAssetDocumentContractValidationResult Rollback =
			Transaction.Rollback(TEXT("Test.SC-ORD-001.Rollback"));
		Passed &= TestTrue(TEXT("SC-ORD-001 late rollback succeeds"), Rollback.bValid);
		Passed &= RequireObservation(*this, TEXT("SC-ORD-001 rollback restores exact target"),
			Before, *Blackboard);
		Passed &= RequireObservation(*this, TEXT("SC-ORD-001 rollback restores exact child"),
			ChildBefore, *Child);
		Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 rollback First"), TEXT("First"), FirstBefore);
		Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 rollback Struct"), TEXT("RetainedStruct"), StructBefore);
		Passed &= RequireSameEntryMetadata(TEXT("SC-ORD-001 rollback Last"), TEXT("Last"), LastBefore);
		Passed &= TestEqual(TEXT("SC-ORD-001 rollback restores exact authored Key IDs"),
			TArray<FBlackboard::FKey>{Blackboard->GetKeyID(TEXT("First")),
				Blackboard->GetKeyID(TEXT("RetainedStruct")), Blackboard->GetKeyID(TEXT("Last"))},
			TArray<FBlackboard::FKey>{FirstBeforeId, StructBeforeId, LastBeforeId});
		Passed &= TestTrue(TEXT("SC-ORD-001 rollback restores SelfActor physical tail"),
			Blackboard->Keys.Num() == 4 && IsEngineDerivedSelfKey(Blackboard->Keys.Last())
				&& Blackboard->Keys.Last().KeyType == SelfBefore.KeyType);
		Passed &= TestEqual(TEXT("SC-ORD-001 rollback restores loaded selector cache"),
			Consumer->Task->DirectSelector.GetSelectedKeyID(), FirstBeforeId);
		TStrongObjectPtr<UBlackboardComponent> Component =
			MakeRemoveConsumerComponent(*this, *Blackboard);
		if (!Component.IsValid()) return false;
		Component->SetValueAsInt(TEXT("Last"), 411);
		Passed &= TestEqual(TEXT("SC-ORD-001 rollback restores ValueOr cache semantics"),
			Consumer->Task->ValueOrInt32.GetValue(Component.Get()), 411);
		Passed &= TestEqual(TEXT("SC-ORD-001 rollback preserves BehaviorTree dirty state"),
			Consumer->Tree->GetOutermost()->IsDirty(), bBeforeConsumerTreeDirty);
	}
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReorderSaveReloadTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Reorder.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReorderSaveReloadTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: a committed real permutation survives an external
	//   save, unload/GC and fresh load with its complete admitted Struct state.
	// - Candidate technique: state-transition characterization across provider
	//   mutation -> commit -> external persistence -> fresh identity readback.
	// - Risk or gap: order can persist while Struct payload, references, child
	//   Key IDs, SelfActor placement or no-op transaction cleanliness diverges.
	// - Next characterization: RetainedAdmittedStructCanonical (SC-ORD-003).
	// - Expected observable outcome: exact literal DTO/order and zero provider
	//   persistence before the external save, then zero-residue fresh readback.
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentAIBlackboardReorder/BlackboardReorderStructReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString ChildPath = PackageName + TEXT(".Child");
	const FString HardObjectPath = PackageName + TEXT(".HardObject");
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PackageFileName, false, true);
	};

	UPackage* Package = CreatePackage(*PackageName);
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* HardObject = NewObject<UBlackboardData>(
		Package, TEXT("HardObject"), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("SC-ORD-003 Blackboard exists"), Blackboard)
		|| !TestNotNull(TEXT("SC-ORD-003 child exists"), Child)
		|| !TestNotNull(TEXT("SC-ORD-003 hard reference exists"), HardObject)) return false;
	TArray<FBlackboardEntry> DerivedKeys = MoveTemp(Blackboard->Keys);
	Blackboard->Keys.Reset();
	AddExistingKey(*Blackboard, TEXT("First"), 1);
	UBlackboardKeyType_Struct* StructKeyType = NewObject<UBlackboardKeyType_Struct>(
		Blackboard, TEXT("RetainedStructKeyType"), RF_Transactional);
	FProperty* DefaultValueProperty = FindFProperty<FProperty>(
		UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	if (!TestNotNull(TEXT("SC-ORD-003 Struct KeyType exists"), StructKeyType)
		|| !TestNotNull(TEXT("SC-ORD-003 DefaultValue property exists"),
			DefaultValueProperty)) return false;
	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	auto& Stored = StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	Stored.ArrayValues = {9, -2};
	Stored.BoolValue = true;
	Stored.ByteValue = 255;
	Stored.ClassValue = UObject::StaticClass();
	Stored.DoubleValue = 12.5;
	Stored.DynamicValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Stored.DynamicValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>().Count = 4;
	Stored.DynamicValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>().Label = TEXT("dynamic");
	Stored.EnumValue = EAssetDocumentAIBlackboardCanonicalStructTestEnum::Second;
	Stored.FloatValue = 1.25f;
	Stored.HardObject = HardObject;
	Stored.Int64Value = MIN_int64 + 7;
	Stored.IntValue = -17;
	Stored.MapValues.Add(TEXT("b"), 2);
	Stored.MapValues.Add(TEXT("a"), 1);
	Stored.NameValue = FName(TEXT("CanonicalName_7"));
	Stored.NestedValue.Count = 3;
	Stored.NestedValue.Label = TEXT("nested");
	Stored.SetValues.Add(TEXT("zulu"));
	Stored.SetValues.Add(TEXT("alpha"));
	Stored.SoftClassValue = TSoftClassPtr<UObject>(UObject::StaticClass());
	Stored.SoftObjectValue = TSoftObjectPtr<UObject>(HardObject);
	Stored.StaticValues[0] = 5;
	Stored.StaticValues[1] = 6;
	Stored.StaticValues[2] = 7;
	Stored.StringValue = TEXT("canonical");
	Stored.TextValue = FText::AsCultureInvariant(TEXT("literal-equivalent"));
	Stored.UInt16Value = MAX_uint16;
	Stored.UInt32Value = MAX_uint32;
	Stored.UInt64Value = MAX_uint64;
	FPropertyChangedEvent DefaultValueChanged(
		DefaultValueProperty, EPropertyChangeType::ValueSet);
	DefaultValueChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(DefaultValueChanged);
	FBlackboardEntry StructEntry;
	StructEntry.EntryName = TEXT("RetainedStruct");
	StructEntry.EntryDescription = TEXT("Retained canonical Struct");
	StructEntry.EntryCategory = TEXT("Retention");
	StructEntry.KeyType = StructKeyType;
	StructEntry.bInstanceSynced = true;
	Blackboard->Keys.Add(MoveTemp(StructEntry));
	AddExistingStockIntKey(*Blackboard, TEXT("Last"), 3);
	Blackboard->Keys.Append(MoveTemp(DerivedKeys));
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Package->SetDirtyFlag(false);

	const auto StructValue = [](const UScriptStruct& Struct, TArray<FString> Keys,
		TArray<FAssetDocumentRuntimeValueDto> Values)
	{
		return ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue(MoveTemp(Keys), MoveTemp(Values)), StringValue(Struct.GetPathName())});
	};
	const FAssetDocumentRuntimeValueDto Nested = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(3), StringValue(TEXT("nested"))});
	const FAssetDocumentRuntimeValueDto Dynamic = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(4), StringValue(TEXT("dynamic"))});
	const FAssetDocumentRuntimeValueDto CanonicalStruct = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct(),
		{TEXT("ArrayValues"), TEXT("BoolValue"), TEXT("ByteValue"), TEXT("ClassValue"),
			TEXT("DoubleValue"), TEXT("DynamicValue"), TEXT("EnumValue"), TEXT("FloatValue"),
			TEXT("HardObject"), TEXT("Int64Value"), TEXT("IntValue"), TEXT("MapValues"),
			TEXT("NameValue"), TEXT("NestedValue"), TEXT("SetValues"), TEXT("SoftClassValue"),
			TEXT("SoftObjectValue"), TEXT("StaticValues"), TEXT("StringValue"), TEXT("TextValue"),
			TEXT("UInt16Value"), TEXT("UInt32Value"), TEXT("UInt64Value")},
		{ArrayValue({IntegerValue(9), IntegerValue(-2)}), BooleanValue(true), IntegerValue(255),
			ReferenceValue(TEXT("ClassRef"), *UObject::StaticClass()), NumberValue(12.5), Dynamic,
			ObjectValue({TEXT("Enum"), TEXT("Enumerator")},
				{StringValue(StaticEnum<EAssetDocumentAIBlackboardCanonicalStructTestEnum>()->GetPathName()),
					StringValue(TEXT("Second"))}),
			NumberValue(1.25), ReferenceValue(TEXT("AssetRef"), *HardObject),
			IntegerValue(MIN_int64 + 7), IntegerValue(-17),
			ArrayValue({ObjectValue({TEXT("Key"), TEXT("Value")},
				{StringValue(TEXT("a")), IntegerValue(1)}),
				ObjectValue({TEXT("Key"), TEXT("Value")},
					{StringValue(TEXT("b")), IntegerValue(2)})}),
			StringValue(TEXT("CanonicalName_7")), Nested,
			ArrayValue({StringValue(TEXT("zulu")), StringValue(TEXT("alpha"))}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftClassRef")), StringValue(UObject::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftAssetRef")), StringValue(HardObjectPath)}),
			ArrayValue({IntegerValue(5), IntegerValue(6), IntegerValue(7)}),
			StringValue(TEXT("canonical")), StringValue(TEXT("literal-equivalent")),
			IntegerValue(MAX_uint16), IntegerValue(MAX_uint32),
			ObjectValue({TEXT("Kind"), TEXT("Value")},
				{StringValue(TEXT("UInt64")), StringValue(TEXT("18446744073709551615"))})});
	const FAssetDocumentRuntimeValueDto ExpectedStructEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("Retention")), StringValue(TEXT("Retained canonical Struct")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {CanonicalStruct}),
			StringValue(TEXT("RetainedStruct")), BooleanValue(true)});
	const FAssetDocumentRuntimeValueDto LastEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("")), StringValue(TEXT("")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Int::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {IntegerValue(3)}),
			StringValue(TEXT("Last")), BooleanValue(false)});
	const FAssetDocumentRuntimeValueDto ExpectedOutput = ObjectValue(
		{TEXT("entries"), TEXT("ordered_local_key_names")},
		{ArrayValue({ExpectedStructEntry, LastEntry,
			MakeEntryReplaceClassV2OriginalEntry(TEXT("First"), 1)}),
			ArrayValue({StringValue(TEXT("RetainedStruct")), StringValue(TEXT("Last")),
				StringValue(TEXT("First"))})});

	bool Passed = true;
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome =
			CreateAssetDocumentAIPrimitiveProvider()->Invoke(
				MakeLocalKeysReorderV2Invocation(Handle.GetValue(), *Blackboard,
					{StringValue(TEXT("RetainedStruct")), StringValue(TEXT("Last")),
						StringValue(TEXT("First"))}), Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("SC-ORD-003 reorder before persistence"), Outcome);
		Passed &= TestTrue(TEXT("SC-ORD-003 returns complete Struct DTO/order"),
			RuntimeValueEquals(Outcome.Value, ExpectedOutput));
		Passed &= TestTrue(TEXT("SC-ORD-003 returns no handles"),
			Outcome.HandleResults.IsEmpty());
		Passed &= RequireExactChangedSet(*this, TEXT("SC-ORD-003 reorder"), Outcome,
			LocalKeysReorderV2Id, Blackboard->GetPathName(),
			FString::Printf(TEXT("asset:%s/Keys.@Order"), *Blackboard->GetPathName()));
		Passed &= TestTrue(TEXT("SC-ORD-003 transaction becomes commit-ready"),
			Transaction.MarkReadyToCommit().bValid);
		Passed &= TestTrue(TEXT("SC-ORD-003 transaction commits before persistence"),
			Transaction.Commit(MakeStructuralCompletionReceipt(Transaction)).bValid);
	}
	Passed &= TestFalse(TEXT("SC-ORD-003 provider performs no persistence"),
		IFileManager::Get().FileExists(*PackageFileName));
	Passed &= TestTrue(TEXT("SC-ORD-003 published package is dirty before external save"),
		Package->IsDirty());
	Package->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	Passed &= TestTrue(TEXT("SC-ORD-003 external lifecycle saves the result"),
		UPackage::SavePackage(Package, Blackboard, *PackageFileName, SaveArgs));
	if (!Passed) return false;
	Passed &= TestTrue(TEXT("SC-ORD-003 package unloads before fresh reload"),
		UPackageTools::UnloadPackages({Package}));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Passed &= TestNull(TEXT("SC-ORD-003 package is absent before fresh load"),
		FindPackage(nullptr, *PackageName));
	UBlackboardData* Reloaded = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	UBlackboardData* ReloadedChild = LoadObject<UBlackboardData>(nullptr, *ChildPath);
	if (!TestNotNull(TEXT("SC-ORD-003 Blackboard fresh-reloads"), Reloaded)
		|| !TestNotNull(TEXT("SC-ORD-003 child fresh-reloads"), ReloadedChild)) return false;
	if (!TestEqual(TEXT("SC-ORD-003 fresh physical cardinality survives"),
		Reloaded->Keys.Num(), 4)) return false;
	Passed &= TestEqual(TEXT("SC-ORD-003 fresh physical order survives"),
		TArray<FName>{Reloaded->Keys[0].EntryName, Reloaded->Keys[1].EntryName,
			Reloaded->Keys[2].EntryName, Reloaded->Keys[3].EntryName},
		TArray<FName>{TEXT("RetainedStruct"), TEXT("Last"), TEXT("First"),
			FBlackboard::KeySelf});
	for (const FName Name : {FName(TEXT("RetainedStruct")), FName(TEXT("Last")),
		FName(TEXT("First"))})
	{
		const FBlackboard::FKey TargetId = Reloaded->GetKeyID(Name);
		const FBlackboard::FKey ChildId = ReloadedChild->GetKeyID(Name);
		Passed &= TestEqual(*(FString(TEXT("SC-ORD-003 fresh target name ")) + Name.ToString()),
			Reloaded->GetKeyName(TargetId), Name);
		Passed &= TestEqual(*(FString(TEXT("SC-ORD-003 fresh child name ")) + Name.ToString()),
			ReloadedChild->GetKeyName(ChildId), Name);
	}
	const FBlackboardEntry* ReloadedStructEntry = Reloaded->Keys.FindByPredicate(
		[](const FBlackboardEntry& Entry)
		{
			return Entry.EntryName == TEXT("RetainedStruct");
		});
	if (!TestNotNull(TEXT("SC-ORD-003 retained Struct fresh-reloads"),
		ReloadedStructEntry)) return false;
	const UBlackboardKeyType_Struct* ReloadedStruct =
		Cast<UBlackboardKeyType_Struct>(ReloadedStructEntry->KeyType);
	if (!TestNotNull(TEXT("SC-ORD-003 exact Struct KeyType fresh-reloads"),
		ReloadedStruct)) return false;
	Passed &= TestTrue(TEXT("SC-ORD-003 exact dynamic Struct identity survives"),
		ReloadedStruct->DefaultValue.GetScriptStruct()
			== FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct());
	const auto* ReloadedPayload = ReloadedStruct->DefaultValue.GetPtr<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	if (!TestNotNull(TEXT("SC-ORD-003 admitted native payload survives"),
		ReloadedPayload)) return false;
	Passed &= TestTrue(TEXT("SC-ORD-003 nested/container/reference values survive"),
		ReloadedPayload->NestedValue.Count == 3
			&& ReloadedPayload->DynamicValue.GetScriptStruct()
				== FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()
			&& ReloadedPayload->MapValues.FindRef(TEXT("a")) == 1
			&& ReloadedPayload->SetValues.Contains(TEXT("alpha"))
			&& GetPathNameSafe(ReloadedPayload->HardObject) == HardObjectPath
			&& ReloadedPayload->SoftObjectValue.ToSoftObjectPath().ToString() == HardObjectPath);
	const FString* ReloadedTextSource = FTextInspector::GetSourceString(
		ReloadedPayload->TextValue);
	Passed &= TestTrue(TEXT("SC-ORD-003 literal-equivalent text survives"),
		ReloadedTextSource != nullptr && *ReloadedTextSource == TEXT("literal-equivalent"));

	const FBlackboardObservation BeforeFreshIdentity(*Reloaded);
	const bool bDirtyBeforeFreshIdentity = Reloaded->GetOutermost()->IsDirty();
	FBlackboardLifecycleTransaction FreshTransaction;
	FBlackboardLifecycleHandleResolver FreshHandles(FreshTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> FreshHandle = ResolveAIMutableAsset(
		*this, *Reloaded, FreshTransaction, FreshHandles);
	if (!FreshHandle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome FreshIdentity =
		CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeLocalKeysReorderV2Invocation(FreshHandle.GetValue(), *Reloaded,
				{StringValue(TEXT("RetainedStruct")), StringValue(TEXT("Last")),
					StringValue(TEXT("First"))}), FreshHandles, &FreshTransaction);
	Passed &= RequireSuccess(*this, TEXT("SC-ORD-003 fresh identity readback"), FreshIdentity);
	Passed &= TestTrue(TEXT("SC-ORD-003 fresh identity returns exact DTO/order"),
		RuntimeValueEquals(FreshIdentity.Value, ExpectedOutput));
	Passed &= TestTrue(TEXT("SC-ORD-003 fresh identity has no output residue"),
		FreshIdentity.HandleResults.IsEmpty() && FreshIdentity.ChangedSet.IsEmpty());
	Passed &= TestTrue(TEXT("SC-ORD-003 fresh identity has no action or footprint"),
		FreshTransaction.ActionEnlistCount == 0
			&& FreshTransaction.FootprintEnlistCount == 0
			&& FreshTransaction.EnlistedFootprint.IsEmpty());
	Passed &= RequireObservation(*this, TEXT("SC-ORD-003 fresh identity preserves state"),
		BeforeFreshIdentity, *Reloaded);
	Passed &= TestEqual(TEXT("SC-ORD-003 fresh identity preserves dirty state"),
		Reloaded->GetOutermost()->IsDirty(), bDirtyBeforeFreshIdentity);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReorderFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Reorder.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReorderFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	using ELoadedFailure = AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ELoadedConsumerCompletionTestFailurePoint;
	// Test design:
	// - Behavior under test: exact local-name preflight is closed, identity avoids
	//   every mutable seam, and a real permutation rolls back each late failure.
	// - Candidate technique: equivalence partitions plus lifecycle transitions.
	// - Risk or gap: a publication failure can retain Struct state, Key IDs, or a
	//   loaded BehaviorTree cache after returning a superficially closed outcome.
	// - Test: RetainedStructClosedAndLateRollback (SC-ORD-002).
	// - Expected observable outcome: zero preflight/identity residue and exactly
	//   one action then footprint for every real late failure before rollback.
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ReorderFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	UBehaviorTree* WrongClass = NewObject<UBehaviorTree>(
		Package, TEXT("WrongClass"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("SC-ORD-002 Blackboard exists"), Blackboard)
		|| !TestNotNull(TEXT("SC-ORD-002 child exists"), Child)
		|| !TestNotNull(TEXT("SC-ORD-002 wrong-class asset exists"), WrongClass)) return false;
	TStrongObjectPtr<UBlackboardData> BlackboardGuard(Blackboard);
	TStrongObjectPtr<UBlackboardData> ChildGuard(Child);
	TStrongObjectPtr<UBehaviorTree> WrongClassGuard(WrongClass);

	TArray<FBlackboardEntry> DerivedKeys = MoveTemp(Blackboard->Keys);
	Blackboard->Keys.Reset();
	AddExistingKey(*Blackboard, TEXT("First"), 21);
	UBlackboardKeyType_Struct* StructKeyType = NewObject<UBlackboardKeyType_Struct>(
		Blackboard, TEXT("RetainedStructKeyType"), RF_Transactional);
	FProperty* DefaultValueProperty = FindFProperty<FProperty>(
		UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	if (!TestNotNull(TEXT("SC-ORD-002 retained Struct KeyType exists"), StructKeyType)
		|| !TestNotNull(TEXT("SC-ORD-002 Struct DefaultValue property exists"),
			DefaultValueProperty)) return false;
	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructNested>();
	auto& Stored = StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>();
	Stored.Count = 17;
	Stored.Label = TEXT("reorder-late-rollback");
	FPropertyChangedEvent StoredChanged(DefaultValueProperty, EPropertyChangeType::ValueSet);
	StoredChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(StoredChanged);
	FBlackboardEntry StructEntry;
	StructEntry.EntryName = TEXT("RetainedStruct");
	StructEntry.EntryDescription = TEXT("Retained failure Struct");
	StructEntry.EntryCategory = TEXT("Retention");
	StructEntry.KeyType = StructKeyType;
	StructEntry.bInstanceSynced = true;
	Blackboard->Keys.Add(MoveTemp(StructEntry));
	AddExistingStockIntKey(*Blackboard, TEXT("Last"), 31);
	Blackboard->Keys.Append(MoveTemp(DerivedKeys));
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Package->SetDirtyFlag(false);
	if (!TestTrue(TEXT("SC-ORD-002 preserves physical SelfActor tail"),
		Blackboard->Keys.Num() == 4 && IsEngineDerivedSelfKey(Blackboard->Keys.Last()))) return false;

	const auto MakeNames = [](std::initializer_list<const TCHAR*> Names)
	{
		TArray<FAssetDocumentRuntimeValueDto> Result;
		Result.Reserve(static_cast<int32>(Names.size()));
		for (const TCHAR* Name : Names) Result.Add(StringValue(Name));
		return Result;
	};
	const auto FindLocalKeyType = [](const UBlackboardData& Owner, const FName Name)
	{
		const FBlackboardEntry* Entry = Owner.Keys.FindByPredicate(
			[Name](const FBlackboardEntry& Candidate)
			{
				return Candidate.EntryName == Name;
			});
		return Entry != nullptr ? Entry->KeyType.Get() : nullptr;
	};
	const auto Invoke = [&](FBlackboardLifecycleTransaction& Transaction,
		FBlackboardLifecycleHandleResolver& Handles,
		TArray<FAssetDocumentRuntimeValueDto> Names)
	{
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return FAssetDocumentPrimitiveOutcome();
		return CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeLocalKeysReorderV2Invocation(Handle.GetValue(), *Blackboard, MoveTemp(Names)),
			Handles, &Transaction);
	};
	const auto RunPreflightFailure = [&](const TCHAR* Label,
		TArray<FAssetDocumentRuntimeValueDto> Names, const TCHAR* ExpectedCode,
		TFunction<void(FAssetDocumentPrimitiveInvocation&)> Mutate = {})
	{
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveInvocation Invocation = MakeLocalKeysReorderV2Invocation(
			Handle.GetValue(), *Blackboard, MoveTemp(Names));
		if (Mutate) Mutate(Invocation);
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			Invocation, Handles, &Transaction);
		return RequireExactFailureTerminal(*this, Label, Outcome, Before, *Blackboard,
			Transaction, ExpectedCode)
			&& RequireObservation(*this, FString(Label) + TEXT(" preserves child"),
				ChildBefore, *Child);
	};

	bool Passed = RunPreflightFailure(TEXT("SC-ORD-002 rejects malformed request"),
		MakeNames({TEXT("First"), TEXT("RetainedStruct"), TEXT("Last")} ),
		TEXT("AIProvider.InvalidValueShape"), [](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments = ObjectValue({TEXT("blackboard_path")},
				{StringValue(TEXT("/Temp/Malformed"))});
		});
	Passed &= RunPreflightFailure(TEXT("SC-ORD-002 rejects invalid transaction"),
		MakeNames({TEXT("First"), TEXT("RetainedStruct"), TEXT("Last")} ),
		TEXT("AIProvider.InvalidTransaction"), [](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.TransactionId = {};
		});
	Passed &= RunPreflightFailure(TEXT("SC-ORD-002 rejects invalid handle"),
		MakeNames({TEXT("First"), TEXT("RetainedStruct"), TEXT("Last")} ),
		TEXT("AIProvider.InvalidHandle"), [](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.HandleArguments[0].Handle.Slot += 999;
		});
	{
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *WrongClass, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeLocalKeysReorderV2Invocation(Handle.GetValue(), *WrongClass,
				MakeNames({TEXT("First")})), Handles, &Transaction);
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("SC-ORD-002 rejects wrong exact asset class"), Outcome, Before, *Blackboard,
			Transaction, TEXT("AIProvider.AuthorityUnavailable"))
			&& RequireObservation(*this,
				TEXT("SC-ORD-002 wrong exact asset class preserves child"), ChildBefore, *Child);
	}
	Passed &= RunPreflightFailure(TEXT("SC-ORD-002 rejects duplicate permutation"),
		MakeNames({TEXT("First"), TEXT("First"), TEXT("Last")} ),
		TEXT("AIProvider.Blackboard.OrderedKeysPermutationInvalid"));
	Passed &= RunPreflightFailure(TEXT("SC-ORD-002 rejects missing permutation"),
		MakeNames({TEXT("First"), TEXT("Last")} ),
		TEXT("AIProvider.Blackboard.OrderedKeysPermutationInvalid"));
	Passed &= RunPreflightFailure(TEXT("SC-ORD-002 rejects extra permutation"),
		MakeNames({TEXT("First"), TEXT("RetainedStruct"), TEXT("Last"), TEXT("Extra")} ),
		TEXT("AIProvider.Blackboard.OrderedKeysPermutationInvalid"));
	Passed &= RunPreflightFailure(TEXT("SC-ORD-002 rejects case-drift permutation"),
		MakeNames({TEXT("first"), TEXT("RetainedStruct"), TEXT("Last")} ),
		TEXT("AIProvider.Blackboard.OrderedKeysPermutationInvalid"));
	Passed &= RunPreflightFailure(TEXT("SC-ORD-002 rejects SelfActor permutation"),
		MakeNames({TEXT("First"), TEXT("RetainedStruct"), TEXT("SelfActor")} ),
		TEXT("AIProvider.Blackboard.OrderedKeysPermutationInvalid"));
	{
		if (!TestTrue(TEXT("SC-ORD-002 child resolves First only through inheritance"),
			Child->GetKeyID(TEXT("First")) != FBlackboard::InvalidKey
			&& Child->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
			{
				return Entry.EntryName == TEXT("First");
			}) == INDEX_NONE)) return false;
		const FBlackboardObservation BeforeChild(*Child);
		const FBlackboardObservation BeforeTarget(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Child, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome =
			CreateAssetDocumentAIPrimitiveProvider()->Invoke(
				MakeLocalKeysReorderV2Invocation(
					Handle.GetValue(), *Child, MakeNames({TEXT("First")})),
				Handles, &Transaction);
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("SC-ORD-002 rejects inherited permutation"), Outcome,
			BeforeChild, *Child, Transaction,
			TEXT("AIProvider.Blackboard.OrderedKeysPermutationInvalid"));
		Passed &= RequireObservation(*this,
			TEXT("SC-ORD-002 inherited permutation preserves parent"),
			BeforeTarget, *Blackboard);
	}

	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructUnsupported>();
	FPropertyChangedEvent UnsupportedChanged(DefaultValueProperty, EPropertyChangeType::ValueSet);
	UnsupportedChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(UnsupportedChanged);
	Package->SetDirtyFlag(false);
	Passed &= RunPreflightFailure(TEXT("SC-ORD-002 closes unsupported retained Struct"),
		MakeNames({TEXT("RetainedStruct"), TEXT("Last"), TEXT("First")} ),
		TEXT("AIProvider.AuthorityUnavailable"));
	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructNested>();
	StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>().Count = 17;
	StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>().Label = TEXT("reorder-late-rollback");
	FPropertyChangedEvent AdmittedChanged(DefaultValueProperty, EPropertyChangeType::ValueSet);
	AdmittedChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(AdmittedChanged);
	Package->SetDirtyFlag(false);

	TUniquePtr<FRemoveLoadedConsumerFixture> Consumer = MakeRemoveLoadedConsumerFixture(
		*this, *Blackboard, TEXT("First"), TEXT("Last"));
	if (!Consumer.IsValid()) return false;
	const FBlackboard::FKey FirstBeforeId = Blackboard->GetKeyID(TEXT("First"));
	const FBlackboard::FKey LastBeforeId = Blackboard->GetKeyID(TEXT("Last"));
	const FBlackboard::FKey StructBeforeId = Blackboard->GetKeyID(TEXT("RetainedStruct"));
	const FBlackboard::FKey SelfBeforeId = Blackboard->GetKeyID(FBlackboard::KeySelf);
	UBlackboardKeyType* const FirstKeyType = FindLocalKeyType(*Blackboard, TEXT("First"));
	UBlackboardKeyType* const StructKeyTypeBefore =
		FindLocalKeyType(*Blackboard, TEXT("RetainedStruct"));
	UBlackboardKeyType* const LastKeyType = FindLocalKeyType(*Blackboard, TEXT("Last"));

	const auto RunIdentityInjection = [&](const TCHAR* Label,
		const EAssetDocumentAIBlackboardTestFailurePoint RowFailure,
		const ELoadedFailure LoadedFailure)
	{
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		if (!PrimeRemoveLoadedConsumer(*this, *Consumer, *Blackboard,
			TEXT("First"), TEXT("Last"), 211)) return false;
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint RowPoint(RowFailure);
			FScopedLoadedConsumerCompletionTestFailurePoint LoadedPoint(LoadedFailure);
			Outcome = Invoke(Transaction, Handles,
				MakeNames({TEXT("First"), TEXT("RetainedStruct"), TEXT("Last")}));
		}
		bool LocalPassed = RequireSuccess(*this, Label, Outcome)
			&& TestTrue(*(FString(Label) + TEXT(" has zero outcome residue")),
				Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty())
			&& TestEqual(*(FString(Label) + TEXT(" enlists zero actions")),
				Transaction.ActionEnlistCount, 0)
			&& TestEqual(*(FString(Label) + TEXT(" enlists zero footprints")),
				Transaction.FootprintEnlistCount, 0)
			&& TestTrue(*(FString(Label) + TEXT(" has no transaction ordering")),
				Transaction.Order.IsEmpty())
			&& RequireObservation(*this, FString(Label) + TEXT(" preserves target"),
				Before, *Blackboard)
			&& RequireObservation(*this, FString(Label) + TEXT(" preserves child"),
				ChildBefore, *Child)
			&& TestEqual(*(FString(Label) + TEXT(" preserves First Key-ID")),
				Blackboard->GetKeyID(TEXT("First")), FirstBeforeId)
			&& TestEqual(*(FString(Label) + TEXT(" preserves Struct Key-ID")),
				Blackboard->GetKeyID(TEXT("RetainedStruct")), StructBeforeId)
			&& TestEqual(*(FString(Label) + TEXT(" preserves Last Key-ID")),
				Blackboard->GetKeyID(TEXT("Last")), LastBeforeId)
			&& TestEqual(*(FString(Label) + TEXT(" preserves SelfActor Key-ID")),
				Blackboard->GetKeyID(FBlackboard::KeySelf), SelfBeforeId)
			&& TestEqual(*(FString(Label) + TEXT(" preserves loaded selector")),
				Consumer->Task->DirectSelector.GetSelectedKeyID(), FirstBeforeId);
		TStrongObjectPtr<UBlackboardComponent> Component =
			MakeRemoveConsumerComponent(*this, *Blackboard);
		if (!Component.IsValid()) return false;
		Component->SetValueAsInt(TEXT("Last"), 211);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" preserves ValueOr cache")),
			Consumer->Task->ValueOrInt32.GetValue(Component.Get()), 211);
		return LocalPassed;
	};
	Passed &= RunIdentityInjection(TEXT("SC-ORD-002 identity ignores publication seam"),
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderPublicationRejected,
		ELoadedFailure::None);
	Passed &= RunIdentityInjection(TEXT("SC-ORD-002 identity ignores before-readback seam"),
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderBeforeReadback,
		ELoadedFailure::None);
	Passed &= RunIdentityInjection(TEXT("SC-ORD-002 identity ignores ChangedSet seam"),
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderChangedSetMismatch,
		ELoadedFailure::None);
	Passed &= RunIdentityInjection(TEXT("SC-ORD-002 identity ignores derived-ID seam"),
		EAssetDocumentAIBlackboardTestFailurePoint::AfterLocalKeyIdRefresh,
		ELoadedFailure::None);
	Passed &= RunIdentityInjection(TEXT("SC-ORD-002 identity ignores loaded consumer before seam"),
		EAssetDocumentAIBlackboardTestFailurePoint::None,
		ELoadedFailure::BeforeLoadedConsumerForwardCompletion);
	Passed &= RunIdentityInjection(TEXT("SC-ORD-002 identity ignores loaded consumer after seam"),
		EAssetDocumentAIBlackboardTestFailurePoint::None,
		ELoadedFailure::AfterLoadedConsumerForwardCompletion);

	int32 ConsumerRuntimeValue = 401;
	const auto RunLateFailure = [&](const TCHAR* Label,
		const EAssetDocumentAIBlackboardTestFailurePoint RowFailure,
		const ELoadedFailure LoadedFailure, const TCHAR* ExpectedCode)
	{
		const int32 PrimedRuntimeValue = ConsumerRuntimeValue++;
		if (!PrimeRemoveLoadedConsumer(*this, *Consumer, *Blackboard,
			TEXT("First"), TEXT("Last"), PrimedRuntimeValue)) return false;
		const bool bBeforeConsumerTreeDirty = Consumer->Tree->GetOutermost()->IsDirty();
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint RowPoint(RowFailure);
			FScopedLoadedConsumerCompletionTestFailurePoint LoadedPoint(LoadedFailure);
			Outcome = Invoke(Transaction, Handles,
				MakeNames({TEXT("RetainedStruct"), TEXT("Last"), TEXT("First")}));
		}
		const bool bExactFailure = Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& Outcome.Diagnostics.Num() == 1 && Outcome.Diagnostics[0].Code == ExpectedCode;
		bool LocalPassed = TestTrue(*(FString(Label) + TEXT(" consumes exact failure seam")),
			bExactFailure);
		if (bExactFailure)
		{
			LocalPassed &= TestTrue(*(FString(Label) + TEXT(" clears output/handles/ChangedSet")),
				Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty()
					&& Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
			LocalPassed &= TestEqual(*(FString(Label) + TEXT(" captures action then footprint")),
				Transaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")});
			LocalPassed &= TestEqual(*(FString(Label) + TEXT(" enlists one footprint")),
				Transaction.EnlistedFootprint.Num(), 1);
		}
		const FAssetDocumentContractValidationResult Rollback = Transaction.Rollback(
			FString(Label) + TEXT(".Rollback"));
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Rollback.Diagnostics)
		{
			AddError(FString::Printf(TEXT("%s rollback diagnostic %s: %s"),
				Label, *Diagnostic.Code, *Diagnostic.Message));
		}
		LocalPassed &= TestTrue(*(FString(Label) + TEXT(" rollback succeeds")), Rollback.bValid);
		LocalPassed &= RequireObservation(*this, FString(Label) + TEXT(" restores target"),
			Before, *Blackboard);
		LocalPassed &= RequireObservation(*this, FString(Label) + TEXT(" restores child"),
			ChildBefore, *Child);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" restores First KeyType")),
			FindLocalKeyType(*Blackboard, TEXT("First")), FirstKeyType);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" restores retained Struct KeyType")),
			FindLocalKeyType(*Blackboard, TEXT("RetainedStruct")), StructKeyTypeBefore);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" restores Last KeyType")),
			FindLocalKeyType(*Blackboard, TEXT("Last")), LastKeyType);
		LocalPassed &= TestTrue(*(FString(Label) + TEXT(" restores physical SelfActor tail")),
			Blackboard->Keys.Num() == 4 && IsEngineDerivedSelfKey(Blackboard->Keys.Last()));
		LocalPassed &= RequireRemoveLoadedConsumerRollback(*this, *Consumer, *Blackboard,
			TEXT("First"), TEXT("Last"), FirstBeforeId, LastBeforeId,
			bBeforeConsumerTreeDirty, PrimedRuntimeValue + 1000);
		return LocalPassed;
	};
	// These six late lanes form the SC-ORD-002 late-failure group. The
	// identity checks above prove that no injection can be consumed before a
	// real mutable permutation reaches its matching lifecycle boundary.
	Passed &= RunLateFailure(TEXT("SC-ORD-002 publication rejection"),
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderPublicationRejected,
		ELoadedFailure::None, TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"));
	Passed &= RunLateFailure(TEXT("SC-ORD-002 loaded consumer before"),
		EAssetDocumentAIBlackboardTestFailurePoint::None,
		ELoadedFailure::BeforeLoadedConsumerForwardCompletion,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunLateFailure(TEXT("SC-ORD-002 loaded consumer after"),
		EAssetDocumentAIBlackboardTestFailurePoint::None,
		ELoadedFailure::AfterLoadedConsumerForwardCompletion,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunLateFailure(TEXT("SC-ORD-002 derived ID late failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::AfterLocalKeyIdRefresh,
		ELoadedFailure::None, TEXT("AIProvider.OperationRejected"));
	Passed &= RunLateFailure(TEXT("SC-ORD-002 before-readback failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderBeforeReadback,
		ELoadedFailure::None, TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunLateFailure(TEXT("SC-ORD-002 ChangedSet mismatch"),
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderChangedSetMismatch,
		ELoadedFailure::None, TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReadbackTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Readback.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReadbackTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: the readback returns one canonical, complete local Blackboard
	//   entry without a handle, transaction, mutation, or retained lease state.
	// - Candidate technique: equivalence classes for local/missing/inherited/
	//   unavailable/malformed inputs plus lifecycle transitions.
	// - Risk or gap: a v1-shaped or HA-backed helper could appear to read an
	//   entry while accepting a handle, an active transaction, or an inherited key.
	// - Test: a zero-property, empty-metadata local entry and each
	//   failure class below.
	// - Expected observable outcome: a six-field DTO on success; otherwise an
	//   empty envelope with no state or footprint change.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2Readback_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Parent = NewObject<UBlackboardData>(
		Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* LocalKeyType =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(Blackboard);
	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* ParentKeyType =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(Parent);
	UAssetDocumentAITypedPreimageTestLocalKeyType* PropertyKeyType =
		NewObject<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard);
	UBlackboardKeyType_Object* ObjectKeyType =
		NewObject<UBlackboardKeyType_Object>(Blackboard);
	UBlackboardKeyType_Object* ObjectClassValueKeyType =
		NewObject<UBlackboardKeyType_Object>(Blackboard);
	UBlackboardKeyType_Object* ObjectInstanceAKeyType =
		NewObject<UBlackboardKeyType_Object>(Blackboard);
	UBlackboardKeyType_Object* ObjectInstanceBKeyType =
		NewObject<UBlackboardKeyType_Object>(Blackboard);
	UBlackboardKeyType_Object* CdoObjectKeyType =
		GetMutableDefault<UBlackboardKeyType_Object>();
	UBlackboardKeyType_Class* ClassKeyType =
		NewObject<UBlackboardKeyType_Class>(Blackboard);
	UBlackboardKeyType_Enum* EnumKeyType =
		NewObject<UBlackboardKeyType_Enum>(Blackboard);
	UBlackboardKeyType_Enum* NullEnumTypeKeyType =
		NewObject<UBlackboardKeyType_Enum>(Blackboard);
	UBlackboardKeyType_Enum* AssetEnumKeyType =
		NewObject<UBlackboardKeyType_Enum>(Blackboard);
	UBlackboardKeyType_Enum* InternalEnumKeyType =
		NewObject<UBlackboardKeyType_Enum>(Blackboard);
	UBlackboardKeyType_Enum* MismatchedNativeEnumKeyType =
		NewObject<UBlackboardKeyType_Enum>(Blackboard);
	UBlackboardKeyType_NativeEnum* NativeEnumKeyType =
		NewObject<UBlackboardKeyType_NativeEnum>(Blackboard);
	UBlackboardKeyType_Struct* StructKeyType =
		NewObject<UBlackboardKeyType_Struct>(Blackboard);
	UBlackboardKeyType_Object* IncompatibleObjectKeyType =
		NewObject<UBlackboardKeyType_Object>(Blackboard);
	UBlackboardKeyType_Class* IncompatibleClassKeyType =
		NewObject<UBlackboardKeyType_Class>(Blackboard);
	UAssetDocumentAITypedPreimageTestKeyType* UnsupportedArrayKeyType =
		NewObject<UAssetDocumentAITypedPreimageTestKeyType>(Blackboard);
	UEnum* const TestEnum = StaticEnum<EAssetDocumentAITypedPreimageTestEnum>();
	UUserDefinedEnum* SparseAssetEnum = NewObject<UUserDefinedEnum>(
		Package, TEXT("SparseAssetEnum"), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("readback Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("readback parent Blackboard fixture exists"), Parent)
		|| !TestNotNull(TEXT("readback zero-property local key type exists"), LocalKeyType)
		|| !TestNotNull(TEXT("readback zero-property inherited key type exists"), ParentKeyType)
		|| !TestNotNull(TEXT("readback property canonicalization key type exists"), PropertyKeyType)
		|| !TestNotNull(TEXT("readback object key type exists"), ObjectKeyType)
		|| !TestNotNull(TEXT("readback object class-value key type exists"), ObjectClassValueKeyType)
		|| !TestNotNull(TEXT("readback first object instance key type exists"), ObjectInstanceAKeyType)
		|| !TestNotNull(TEXT("readback second object instance key type exists"), ObjectInstanceBKeyType)
		|| !TestNotNull(TEXT("readback object CDO rejection fixture exists"), CdoObjectKeyType)
		|| !TestNotNull(TEXT("readback class key type exists"), ClassKeyType)
		|| !TestNotNull(TEXT("readback enum key type exists"), EnumKeyType)
		|| !TestNotNull(TEXT("readback null-enum-type key type exists"), NullEnumTypeKeyType)
		|| !TestNotNull(TEXT("readback asset enum key type exists"), AssetEnumKeyType)
		|| !TestNotNull(TEXT("readback internal enum key type exists"), InternalEnumKeyType)
		|| !TestNotNull(TEXT("readback mismatched native enum key type exists"), MismatchedNativeEnumKeyType)
		|| !TestNotNull(TEXT("readback native enum key type exists"), NativeEnumKeyType)
		|| !TestNotNull(TEXT("readback struct key type exists"), StructKeyType)
		|| !TestNotNull(TEXT("readback incompatible object key type exists"), IncompatibleObjectKeyType)
		|| !TestNotNull(TEXT("readback incompatible class key type exists"), IncompatibleClassKeyType)
		|| !TestNotNull(TEXT("readback unsupported-array key type exists"), UnsupportedArrayKeyType)
		|| !TestNotNull(TEXT("readback reflected enum exists"), TestEnum)
		|| !TestNotNull(TEXT("readback sparse asset enum exists"), SparseAssetEnum))
	{
		return false;
	}
	const FString SparseEnumScope = SparseAssetEnum->GetPathName() + TEXT("::");
	TArray<TPair<FName, int64>> SparseEnumNames{
		{FName(*(SparseEnumScope + TEXT("First"))), 3},
		{FName(*(SparseEnumScope + TEXT("Sparse"))), 17}};
	if (!TestTrue(TEXT("readback sparse asset enum fixture accepts non-contiguous values"),
		AssetDocumentEnums::SetEnums(*SparseAssetEnum, SparseEnumNames, UEnum::ECppForm::Namespaced,
			EEnumFlags::None, true)))
	{
		return false;
	}

	FBlackboardEntry LocalEntry;
	LocalEntry.EntryName = TEXT("OwnedLocal");
	LocalEntry.KeyType = LocalKeyType;
	LocalEntry.EntryDescription = TEXT("");
	LocalEntry.EntryCategory = NAME_None;
	LocalEntry.bInstanceSynced = false;
	Blackboard->Keys.Add(LocalEntry);
	PropertyKeyType->Int32Value = 41;
	PropertyKeyType->StringValue = TEXT("Canonical value");
	FBlackboardEntry PropertyEntry;
	PropertyEntry.EntryName = TEXT("PropertyCanonical");
	PropertyEntry.KeyType = PropertyKeyType;
	PropertyEntry.EntryDescription = TEXT("Canonical properties");
	PropertyEntry.EntryCategory = TEXT("Readback");
	PropertyEntry.bInstanceSynced = true;
	Blackboard->Keys.Add(PropertyEntry);
	ObjectKeyType->BaseClass = AActor::StaticClass();
	ObjectKeyType->DefaultValue = nullptr;
	FBlackboardEntry ObjectEntry;
	ObjectEntry.EntryName = TEXT("ObjectCanonical");
	ObjectEntry.KeyType = ObjectKeyType;
	Blackboard->Keys.Add(ObjectEntry);
	ObjectClassValueKeyType->BaseClass = UClass::StaticClass();
	ObjectClassValueKeyType->DefaultValue = APawn::StaticClass();
	FBlackboardEntry ObjectClassValueEntry;
	ObjectClassValueEntry.EntryName = TEXT("ObjectClassValue");
	ObjectClassValueEntry.KeyType = ObjectClassValueKeyType;
	Blackboard->Keys.Add(ObjectClassValueEntry);
	ObjectInstanceAKeyType->BaseClass = UBlackboardData::StaticClass();
	ObjectInstanceAKeyType->DefaultValue = Blackboard;
	FBlackboardEntry ObjectInstanceAEntry;
	ObjectInstanceAEntry.EntryName = TEXT("ObjectInstanceA");
	ObjectInstanceAEntry.KeyType = ObjectInstanceAKeyType;
	Blackboard->Keys.Add(ObjectInstanceAEntry);
	ObjectInstanceBKeyType->BaseClass = UBlackboardData::StaticClass();
	ObjectInstanceBKeyType->DefaultValue = Parent;
	FBlackboardEntry ObjectInstanceBEntry;
	ObjectInstanceBEntry.EntryName = TEXT("ObjectInstanceB");
	ObjectInstanceBEntry.KeyType = ObjectInstanceBKeyType;
	Blackboard->Keys.Add(ObjectInstanceBEntry);
	FBlackboardEntry CdoObjectEntry;
	CdoObjectEntry.EntryName = TEXT("CdoObjectForbidden");
	CdoObjectEntry.KeyType = CdoObjectKeyType;
	Blackboard->Keys.Add(CdoObjectEntry);
	ClassKeyType->BaseClass = AActor::StaticClass();
	ClassKeyType->DefaultValue = APawn::StaticClass();
	FBlackboardEntry ClassEntry;
	ClassEntry.EntryName = TEXT("ClassCanonical");
	ClassEntry.KeyType = ClassKeyType;
	Blackboard->Keys.Add(ClassEntry);
	IncompatibleObjectKeyType->BaseClass = APawn::StaticClass();
	IncompatibleObjectKeyType->DefaultValue = AActor::StaticClass()->GetDefaultObject();
	FBlackboardEntry IncompatibleObjectEntry;
	IncompatibleObjectEntry.EntryName = TEXT("IncompatibleObject");
	IncompatibleObjectEntry.KeyType = IncompatibleObjectKeyType;
	Blackboard->Keys.Add(IncompatibleObjectEntry);
	IncompatibleClassKeyType->BaseClass = APawn::StaticClass();
	IncompatibleClassKeyType->DefaultValue = AActor::StaticClass();
	FBlackboardEntry IncompatibleClassEntry;
	IncompatibleClassEntry.EntryName = TEXT("IncompatibleClass");
	IncompatibleClassEntry.KeyType = IncompatibleClassKeyType;
	Blackboard->Keys.Add(IncompatibleClassEntry);
	EnumKeyType->EnumType = TestEnum;
	EnumKeyType->EnumName = TestEnum->GetPathName();
	EnumKeyType->bIsEnumNameValid = true;
	EnumKeyType->DefaultValue = static_cast<uint8>(EAssetDocumentAITypedPreimageTestEnum::Replacement);
	FBlackboardEntry EnumEntry;
	EnumEntry.EntryName = TEXT("EnumCanonical");
	EnumEntry.KeyType = EnumKeyType;
	Blackboard->Keys.Add(EnumEntry);
	FBlackboardEntry NullEnumTypeEntry;
	NullEnumTypeEntry.EntryName = TEXT("NullEnumType");
	NullEnumTypeEntry.KeyType = NullEnumTypeKeyType;
	Blackboard->Keys.Add(NullEnumTypeEntry);
	AssetEnumKeyType->EnumType = SparseAssetEnum;
	AssetEnumKeyType->EnumName = TEXT("");
	AssetEnumKeyType->DefaultValue = 17;
	FBlackboardEntry AssetEnumEntry;
	AssetEnumEntry.EntryName = TEXT("AssetEnumCanonical");
	AssetEnumEntry.KeyType = AssetEnumKeyType;
	Blackboard->Keys.Add(AssetEnumEntry);
	InternalEnumKeyType->EnumType = SparseAssetEnum;
	InternalEnumKeyType->EnumName = TEXT("");
	InternalEnumKeyType->DefaultValue = 18;
	FBlackboardEntry InternalEnumEntry;
	InternalEnumEntry.EntryName = TEXT("InternalEnumForbidden");
	InternalEnumEntry.KeyType = InternalEnumKeyType;
	Blackboard->Keys.Add(InternalEnumEntry);
	MismatchedNativeEnumKeyType->EnumType = TestEnum;
	MismatchedNativeEnumKeyType->EnumName = TEXT("/Script/AssetDocumentAI.WrongEnum");
	MismatchedNativeEnumKeyType->bIsEnumNameValid = true;
	MismatchedNativeEnumKeyType->DefaultValue =
		static_cast<uint8>(EAssetDocumentAITypedPreimageTestEnum::Replacement);
	FBlackboardEntry MismatchedNativeEnumEntry;
	MismatchedNativeEnumEntry.EntryName = TEXT("MismatchedNativeEnum");
	MismatchedNativeEnumEntry.KeyType = MismatchedNativeEnumKeyType;
	Blackboard->Keys.Add(MismatchedNativeEnumEntry);
	FBlackboardEntry NativeEnumEntry;
	NativeEnumEntry.EntryName = TEXT("NativeEnumForbidden");
	NativeEnumEntry.KeyType = NativeEnumKeyType;
	Blackboard->Keys.Add(NativeEnumEntry);
	FBlackboardEntry StructEntry;
	StructKeyType->DefaultValue.InitializeAs<FVector>(1.0, 2.0, 3.0);
	StructEntry.EntryName = TEXT("StructCanonical");
	StructEntry.KeyType = StructKeyType;
	Blackboard->Keys.Add(StructEntry);
	FBlackboardEntry ArrayEntry;
	ArrayEntry.EntryName = TEXT("UnsupportedArray");
	ArrayEntry.KeyType = UnsupportedArrayKeyType;
	Blackboard->Keys.Add(ArrayEntry);
	FBlackboardEntry InheritedEntry;
	InheritedEntry.EntryName = TEXT("InheritedOnly");
	InheritedEntry.KeyType = ParentKeyType;
	Parent->Keys.Add(InheritedEntry);
	Blackboard->Parent = Parent;
	Package->SetDirtyFlag(false);

	bool Passed = true;
	FBlackboardLifecycleTransaction Transaction;
	FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
	const FBlackboardObservation Before(*Blackboard);
	for (const EAssetDocumentRuntimeRequestPhase Phase : {
		EAssetDocumentRuntimeRequestPhase::Validate,
		EAssetDocumentRuntimeRequestPhase::Preflight,
		EAssetDocumentRuntimeRequestPhase::Verify})
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(),
				TEXT("OwnedLocal"), Phase), Handles, nullptr);
		Passed &= RequireCanonicalReadbackEntry(*this, Outcome, *Blackboard, TEXT("OwnedLocal"));
		Passed &= TestTrue(TEXT("readback phase success leaves no footprint or authored mutation"),
			Before.Equals(*Blackboard) && Transaction.EnlistedFootprint.IsEmpty());
	}
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(),
				TEXT("PropertyCanonical")), Handles, nullptr);
		Passed &= RequireCanonicalPropertyReadback(*this, Outcome);
		Passed &= TestTrue(TEXT("readback property canonicalization has no mutation footprint"),
			Before.Equals(*Blackboard) && Transaction.EnlistedFootprint.IsEmpty());
	}
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(),
				TEXT("ObjectCanonical")), Handles, nullptr);
		const bool bCanonical = RequireCanonicalSpecialKeyReadback(*this,
			TEXT("readback Object KeyType canonicalization"), Outcome, *UBlackboardKeyType_Object::StaticClass(),
			TEXT("ObjectCanonical"), {TEXT("BaseClass"), TEXT("DefaultValue")});
		Passed &= bCanonical;
		if (bCanonical)
		{
			const FAssetDocumentRuntimeValueDto& Properties = Outcome.Value.Elements[0].Elements[3];
			Passed &= RequireCanonicalReference(*this, TEXT("readback Object BaseClass"), Properties.Elements[0],
				TEXT("ClassRef"), AActor::StaticClass()->GetPathName());
			Passed &= TestEqual(TEXT("readback Object null DefaultValue is canonical null"),
				Properties.Elements[1].Kind, EAssetDocumentRuntimeValueKind::Null);
		}
		Passed &= TestTrue(TEXT("readback Object canonicalization has no footprint"),
			Before.Equals(*Blackboard) && Transaction.EnlistedFootprint.IsEmpty());
	}
	struct FObjectDefaultReadbackCase
	{
		const TCHAR* EntryName;
		const TCHAR* Label;
		const UClass* BaseClass;
		const UObject* DefaultValue;
	};
	for (const FObjectDefaultReadbackCase& ObjectCase : {
		FObjectDefaultReadbackCase{TEXT("ObjectClassValue"), TEXT("readback Object UClass-value canonicalization"),
			UClass::StaticClass(), APawn::StaticClass()},
		FObjectDefaultReadbackCase{TEXT("ObjectInstanceA"), TEXT("readback first live object default"),
			UBlackboardData::StaticClass(), Blackboard},
		FObjectDefaultReadbackCase{TEXT("ObjectInstanceB"), TEXT("readback second live object default"),
			UBlackboardData::StaticClass(), Parent}})
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(), ObjectCase.EntryName),
			Handles, nullptr);
		const bool bCanonical = RequireCanonicalSpecialKeyReadback(*this, ObjectCase.Label,
			Outcome, *UBlackboardKeyType_Object::StaticClass(), ObjectCase.EntryName,
			{TEXT("BaseClass"), TEXT("DefaultValue")});
		Passed &= bCanonical;
		if (bCanonical)
		{
			const FAssetDocumentRuntimeValueDto& Properties = Outcome.Value.Elements[0].Elements[3];
			Passed &= RequireCanonicalReference(*this, FString(ObjectCase.Label) + TEXT(" BaseClass"),
				Properties.Elements[0], TEXT("ClassRef"), ObjectCase.BaseClass->GetPathName());
			Passed &= RequireCanonicalReference(*this, FString(ObjectCase.Label) + TEXT(" DefaultValue"),
				Properties.Elements[1], TEXT("AssetRef"), ObjectCase.DefaultValue->GetPathName());
		}
		Passed &= TestTrue(*(FString(ObjectCase.Label) + TEXT(" has no footprint")),
			Before.Equals(*Blackboard) && Transaction.EnlistedFootprint.IsEmpty());
	}
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(),
				TEXT("ClassCanonical")), Handles, nullptr);
		const bool bCanonical = RequireCanonicalSpecialKeyReadback(*this,
			TEXT("readback Class KeyType canonicalization"), Outcome, *UBlackboardKeyType_Class::StaticClass(),
			TEXT("ClassCanonical"), {TEXT("BaseClass"), TEXT("DefaultValue")});
		Passed &= bCanonical;
		if (bCanonical)
		{
			const FAssetDocumentRuntimeValueDto& Properties = Outcome.Value.Elements[0].Elements[3];
			Passed &= RequireCanonicalReference(*this, TEXT("readback Class BaseClass"), Properties.Elements[0],
				TEXT("ClassRef"), AActor::StaticClass()->GetPathName());
			Passed &= RequireCanonicalReference(*this, TEXT("readback Class DefaultValue"), Properties.Elements[1],
				TEXT("ClassRef"), APawn::StaticClass()->GetPathName());
		}
		Passed &= TestTrue(TEXT("readback Class canonicalization has no footprint"),
			Before.Equals(*Blackboard) && Transaction.EnlistedFootprint.IsEmpty());
	}
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(),
				TEXT("EnumCanonical")), Handles, nullptr);
		const bool bCanonical = RequireCanonicalSpecialKeyReadback(*this,
			TEXT("readback Enum KeyType canonicalization"), Outcome, *UBlackboardKeyType_Enum::StaticClass(),
			TEXT("EnumCanonical"), {TEXT("DefaultValue"), TEXT("EnumName"), TEXT("EnumType")});
		Passed &= bCanonical;
		if (bCanonical)
		{
			const FAssetDocumentRuntimeValueDto& Properties = Outcome.Value.Elements[0].Elements[3];
			Passed &= TestEqual(TEXT("readback Enum DefaultValue is the enumerator name"),
				Properties.Elements[0].StringValue, FString(TEXT("Replacement")));
			Passed &= TestEqual(TEXT("readback EnumName is the canonical enum path"),
				Properties.Elements[1].StringValue, TestEnum->GetPathName());
			Passed &= RequireCanonicalReference(*this, TEXT("readback EnumType"), Properties.Elements[2],
				TEXT("AssetRef"), TestEnum->GetPathName());
		}
		Passed &= TestTrue(TEXT("readback Enum canonicalization has no footprint"),
			Before.Equals(*Blackboard) && Transaction.EnlistedFootprint.IsEmpty());
	}
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(),
				TEXT("AssetEnumCanonical")), Handles, nullptr);
		const bool bCanonical = RequireCanonicalSpecialKeyReadback(*this,
			TEXT("readback sparse asset Enum canonicalization"), Outcome,
			*UBlackboardKeyType_Enum::StaticClass(), TEXT("AssetEnumCanonical"),
			{TEXT("DefaultValue"), TEXT("EnumName"), TEXT("EnumType")});
		Passed &= bCanonical;
		if (bCanonical)
		{
			const FAssetDocumentRuntimeValueDto& Properties = Outcome.Value.Elements[0].Elements[3];
			Passed &= TestEqual(TEXT("readback sparse Enum DefaultValue is the literal logical enumerator"),
				Properties.Elements[0].StringValue, FString(TEXT("Sparse")));
			Passed &= TestEqual(TEXT("readback asset-backed EnumName remains empty"),
				Properties.Elements[1].StringValue, FString());
			Passed &= RequireCanonicalReference(*this, TEXT("readback sparse asset EnumType"),
				Properties.Elements[2], TEXT("AssetRef"), SparseAssetEnum->GetPathName());
		}
		Passed &= TestTrue(TEXT("readback sparse asset Enum canonicalization has no footprint"),
			Before.Equals(*Blackboard) && Transaction.EnlistedFootprint.IsEmpty());
	}

	// The readback must be independent of prior request/lease lifecycle: the second
	// canonical observation has fresh invocation identity and cannot reuse a
	// retained UObject, KeyType, Key ID, cache, or handle.
	{
		FBlackboardLifecycleTransaction RepeatedTransaction;
		FBlackboardLifecycleHandleResolver RepeatedHandles(
			RepeatedTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Repeated = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, RepeatedTransaction.GetMutationToken(),
				TEXT("OwnedLocal")), RepeatedHandles, nullptr);
		Passed &= RequireCanonicalReadbackEntry(*this, Repeated, *Blackboard, TEXT("OwnedLocal"));
		Passed &= TestTrue(TEXT("readback repeated readback has no retained footprint"),
			RepeatedTransaction.EnlistedFootprint.IsEmpty() && Before.Equals(*Blackboard));
	}

	{
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(),
				TEXT("StructCanonical")), Handles, nullptr);
		const FAssetDocumentRuntimeValueDto Expected = ObjectValue(
			{TEXT("entry")},
			{ObjectValue(
				{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
					TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
				{StringValue(TEXT("")), StringValue(TEXT("")),
					ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
					ObjectValue(
						{TEXT("DefaultValue")},
						{ObjectValue(
							{TEXT("Properties"), TEXT("Struct")},
							{ObjectValue(
								{TEXT("X"), TEXT("Y"), TEXT("Z")},
								{NumberValue(1.0), NumberValue(2.0), NumberValue(3.0)}),
								StringValue(TEXT("/Script/CoreUObject.Vector"))})}),
					StringValue(TEXT("StructCanonical")), BooleanValue(false)})});
		Passed &= TestEqual(TEXT("ExactStructCanonical succeeds"), Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		Passed &= TestTrue(TEXT("ExactStructCanonical emits the exact complete canonical DTO"),
			RuntimeValueEquals(Outcome.Value, Expected));
		Passed &= TestTrue(TEXT("ExactStructCanonical has no handles, ChangedSet, footprint or mutation"),
			Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty()
				&& Transaction.EnlistedFootprint.IsEmpty() && Before.Equals(*Blackboard));
	}

	for (const FString& UnsupportedName : {FString(TEXT("UnsupportedArray"))})
	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, FailureTransaction.GetMutationToken(), UnsupportedName),
			FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this,
			FString::Printf(TEXT("readback unsupported public %s property"), *UnsupportedName), Outcome,
			Before, *Blackboard, FailureTransaction,
			TEXT("AIProvider.AuthorityUnavailable"));
	}

	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, FailureTransaction.GetMutationToken(),
				TEXT("NativeEnumForbidden")), FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback NativeEnum KeyType"), Outcome,
			Before, *Blackboard, FailureTransaction,
			TEXT("AIProvider.AuthorityUnavailable"));
	}
	for (const TCHAR* IncompatibleName : {TEXT("IncompatibleObject"), TEXT("IncompatibleClass")})
	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, FailureTransaction.GetMutationToken(), IncompatibleName),
			FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this,
			FString::Printf(TEXT("readback incompatible %s default"), IncompatibleName), Outcome,
			Before, *Blackboard, FailureTransaction, TEXT("AIProvider.AuthorityUnavailable"));
	}
	for (const TCHAR* InvalidEnumName : {
		TEXT("InternalEnumForbidden"), TEXT("MismatchedNativeEnum"), TEXT("NullEnumType")})
	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, FailureTransaction.GetMutationToken(), InvalidEnumName),
			FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this,
			FString::Printf(TEXT("readback invalid exact Enum %s"), InvalidEnumName), Outcome,
			Before, *Blackboard, FailureTransaction, TEXT("AIProvider.AuthorityUnavailable"));
	}
	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, FailureTransaction.GetMutationToken(),
				TEXT("CdoObjectForbidden")), FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback exact stock KeyType CDO"), Outcome,
			Before, *Blackboard, FailureTransaction, TEXT("AIProvider.AuthorityUnavailable"));
	}

	struct FForbiddenKeyTypeClassCase
	{
		const TCHAR* Label;
		EClassFlags Flag;
	};
	for (const FForbiddenKeyTypeClassCase& ForbiddenCase : {
		FForbiddenKeyTypeClassCase{TEXT("abstract"), CLASS_Abstract},
		FForbiddenKeyTypeClassCase{TEXT("deprecated"), CLASS_Deprecated},
		FForbiddenKeyTypeClassCase{TEXT("superseded"), CLASS_NewerVersionExists}})
	{
		FScopedKeyTypeClassFlags ScopedFlags(*LocalKeyType->GetClass(), ForbiddenCase.Flag);
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, FailureTransaction.GetMutationToken(), TEXT("OwnedLocal")),
			FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this,
			FString::Printf(TEXT("readback forbidden %s KeyType class"), ForbiddenCase.Label), Outcome,
			Before, *Blackboard, FailureTransaction,
			TEXT("AIProvider.AuthorityUnavailable"));
	}

	for (const TCHAR* MissingName : {TEXT("MissingLocal"), TEXT("InheritedOnly")})
	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, FailureTransaction.GetMutationToken(), MissingName),
			FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this, FString::Printf(TEXT("readback %s key"), MissingName),
			Outcome, Before, *Blackboard, FailureTransaction,
			FCString::Strcmp(MissingName, TEXT("InheritedOnly")) == 0
				? TEXT("AIProvider.Blackboard.KeyInherited")
				: TEXT("AIProvider.Blackboard.KeyMissing"));
	}

	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		FAssetDocumentPrimitiveInvocation UnavailableInvocation =
			MakeEntryReadbackInvocation(*Blackboard, FailureTransaction.GetMutationToken(), TEXT("OwnedLocal"));
		UnavailableInvocation.ValueArguments.Elements[0].StringValue =
			TEXT("/Temp/AssetDocumentAI/NoSuchBlackboard.NoSuchBlackboard");
		const FAssetDocumentPrimitiveOutcome MissingTarget = Provider->Invoke(
			UnavailableInvocation, FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback unavailable Blackboard target"),
			MissingTarget, Before, *Blackboard, FailureTransaction,
			TEXT("AIProvider.Blackboard.EntryReadbackUnavailable"));
	}

	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		FAssetDocumentPrimitiveInvocation Malformed = MakeEntryReadbackInvocation(
			*Blackboard, FailureTransaction.GetMutationToken(), TEXT("OwnedLocal"));
		Malformed.ValueArguments = ObjectValue({TEXT("blackboard_path")},
			{StringValue(Blackboard->GetPathName())});
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Malformed, FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback malformed envelope"), Outcome,
			Before, *Blackboard, FailureTransaction);
	}

	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		FAssetDocumentPrimitiveInvocation SuppliedHandle = MakeEntryReadbackInvocation(
			*Blackboard, FailureTransaction.GetMutationToken(), TEXT("OwnedLocal"));
		FAssetDocumentOpaqueHandle Unexpected;
		Unexpected.RequestId = FailureTransaction.GetMutationToken().RequestId;
		Unexpected.TransactionId = FailureTransaction.GetMutationToken().TransactionId;
		Unexpected.RuntimeRevision = FailureTransaction.GetMutationToken().RuntimeRevision;
		Unexpected.ProviderId = TEXT("ai");
		Unexpected.Slot = 1;
		Unexpected.Generation = 1;
		Unexpected.Kind = EAssetDocumentHandleKind::Asset;
		Unexpected.Access = EAssetDocumentHandleAccess::Mutable;
		SuppliedHandle.HandleArguments = {{TEXT("asset"), Unexpected}};
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			SuppliedHandle, FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback supplied handle"), Outcome,
			Before, *Blackboard, FailureTransaction);
	}

	{
		FBlackboardLifecycleTransaction ActiveTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(ActiveTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, ActiveTransaction.GetMutationToken(), TEXT("OwnedLocal")),
			FailureHandles, &ActiveTransaction);
		Passed &= RequireReadbackFailure(*this, TEXT("readback active transaction"), Outcome,
			Before, *Blackboard, ActiveTransaction,
			TEXT("AIProvider.InvalidTransaction"));
	}

	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* NonBlackboard =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(
			Package, TEXT("NonBlackboard"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("readback loaded non-Blackboard fixture exists"), NonBlackboard))
	{
		return false;
	}
	{
		FBlackboardLifecycleTransaction FailureTransaction;
		FBlackboardLifecycleHandleResolver FailureHandles(FailureTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*NonBlackboard, FailureTransaction.GetMutationToken(), TEXT("OwnedLocal")),
			FailureHandles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback non-Blackboard target"), Outcome,
			Before, *Blackboard, FailureTransaction,
			TEXT("AIProvider.Blackboard.EntryReadbackUnavailable"));
	}

	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReadbackFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Readback.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReadbackFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: every readback rejection is failure-closed and cannot
	//   create a handle, ChangedSet, transaction footprint, or authored change.
	// - Candidate technique: boundary/error partitioning across entry identity,
	//   invocation envelope, runtime context, transaction and target class.
	// - Risk or gap: a null KeyType or a wrapped public FKey lookup can leave a
	//   local array match whose UBlackboardData::GetKey identity is different.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ReadbackFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* OwnedKeyType =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(Blackboard);
	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* NonBlackboard =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(Package);
	UBlackboardKeyType_Struct* UnsupportedStructKeyType =
		NewObject<UBlackboardKeyType_Struct>(Blackboard);
	if (!TestNotNull(TEXT("readback failure Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("readback failure owned KeyType fixture exists"), OwnedKeyType)
		|| !TestNotNull(TEXT("readback failure non-Blackboard fixture exists"), NonBlackboard)
		|| !TestNotNull(TEXT("readback failure unsupported Struct fixture exists"), UnsupportedStructKeyType))
	{
		return false;
	}

	FBlackboardEntry OwnedEntry;
	OwnedEntry.EntryName = TEXT("OwnedLocal");
	OwnedEntry.KeyType = OwnedKeyType;
	Blackboard->Keys.Add(OwnedEntry);
	FBlackboardEntry NullKeyTypeEntry;
	NullKeyTypeEntry.EntryName = TEXT("NullKeyType");
	Blackboard->Keys.Add(NullKeyTypeEntry);
	UnsupportedStructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructUnsupported>();
	FBlackboardEntry UnsupportedStructEntry;
	UnsupportedStructEntry.EntryName = TEXT("StructCanonicalClosed");
	UnsupportedStructEntry.KeyType = UnsupportedStructKeyType;
	Blackboard->Keys.Add(UnsupportedStructEntry);

	// Duplicate local names violate the frozen local-name identity contract.
	// GetKey/GetKeyID expose the first entry; the complete owned-local scan must
	// detect the residual second identity and reject the ambiguous entry.
	FBlackboardEntry ResidualIdentityVisible;
	ResidualIdentityVisible.EntryName = TEXT("ResidualIdentityMismatch");
	ResidualIdentityVisible.KeyType = OwnedKeyType;
	Blackboard->Keys.Add(ResidualIdentityVisible);
	FBlackboardEntry ResidualIdentityLocal;
	ResidualIdentityLocal.EntryName = TEXT("ResidualIdentityMismatch");
	ResidualIdentityLocal.KeyType = OwnedKeyType;
	Blackboard->Keys.Add(ResidualIdentityLocal);
	Package->SetDirtyFlag(false);

	bool Passed = true;
	const FBlackboardObservation Before(*Blackboard);
	for (const TCHAR* EntryName : {TEXT("NullKeyType"), TEXT("ResidualIdentityMismatch"),
		TEXT("StructCanonicalClosed")})
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(), EntryName),
			Handles, nullptr);
		Passed &= RequireReadbackFailure(*this,
			FString::Printf(TEXT("readback %s entry identity"), EntryName), Outcome,
			Before, *Blackboard, Transaction, TEXT("AIProvider.AuthorityUnavailable"));
	}

	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		FAssetDocumentPrimitiveInvocation WrongPhase = MakeEntryReadbackInvocation(
			*Blackboard, Transaction.GetMutationToken(), TEXT("OwnedLocal"),
			EAssetDocumentRuntimeRequestPhase::Apply);
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(WrongPhase, Handles, nullptr);
		Passed &= RequireExactFailureTerminal(*this, TEXT("readback disallowed Apply phase"), Outcome,
			Before, *Blackboard, Transaction, TEXT("AIProvider.InvalidContext"));
	}

	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const bool bPreviousEditor = GIsEditor;
		GIsEditor = false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, Transaction.GetMutationToken(), TEXT("OwnedLocal")),
			Handles, nullptr);
		GIsEditor = bPreviousEditor;
		Passed &= RequireReadbackFailure(*this, TEXT("readback non-Editor context"), Outcome,
			Before, *Blackboard, Transaction, TEXT("AIProvider.InvalidContext"));
	}

	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		FAssetDocumentPrimitiveInvocation Malformed = MakeEntryReadbackInvocation(
			*Blackboard, Transaction.GetMutationToken(), TEXT("OwnedLocal"));
		Malformed.ValueArguments = ObjectValue(
			{TEXT("blackboard_path")}, {StringValue(Blackboard->GetPathName())});
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Malformed, Handles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback malformed failure envelope"), Outcome,
			Before, *Blackboard, Transaction);
	}

	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		FAssetDocumentPrimitiveInvocation InvalidRuntimeValue = MakeEntryReadbackInvocation(
			*Blackboard, Transaction.GetMutationToken(), TEXT("OwnedLocal"));
		InvalidRuntimeValue.ValueArguments = {};
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			InvalidRuntimeValue, Handles, nullptr);
		Passed &= RequireExactFailureTerminal(*this, TEXT("readback invalid Runtime value"), Outcome,
			Before, *Blackboard, Transaction, TEXT("AIProvider.InvalidValueShape"));
	}

	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		FAssetDocumentPrimitiveInvocation WithHandle = MakeEntryReadbackInvocation(
			*Blackboard, Transaction.GetMutationToken(), TEXT("OwnedLocal"));
		FAssetDocumentOpaqueHandle Unexpected;
		Unexpected.RequestId = Transaction.GetMutationToken().RequestId;
		Unexpected.TransactionId = Transaction.GetMutationToken().TransactionId;
		Unexpected.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
		Unexpected.ProviderId = TEXT("ai");
		Unexpected.Slot = 1;
		Unexpected.Generation = 1;
		Unexpected.Kind = EAssetDocumentHandleKind::Asset;
		Unexpected.Access = EAssetDocumentHandleAccess::Mutable;
		WithHandle.HandleArguments = {{TEXT("asset"), Unexpected}};
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(WithHandle, Handles, nullptr);
		Passed &= RequireExactFailureTerminal(*this, TEXT("readback supplied-handle failure envelope"), Outcome,
			Before, *Blackboard, Transaction, TEXT("AIProvider.InvalidHandle"));
	}

	{
		FBlackboardLifecycleTransaction ActiveTransaction;
		FBlackboardLifecycleHandleResolver Handles(ActiveTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*Blackboard, ActiveTransaction.GetMutationToken(), TEXT("OwnedLocal")),
			Handles, &ActiveTransaction);
		Passed &= RequireReadbackFailure(*this, TEXT("readback active-transaction failure envelope"), Outcome,
			Before, *Blackboard, ActiveTransaction, TEXT("AIProvider.InvalidTransaction"));
	}

	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		FAssetDocumentPrimitiveInvocation Unavailable = MakeEntryReadbackInvocation(
			*Blackboard, Transaction.GetMutationToken(), TEXT("OwnedLocal"));
		Unavailable.ValueArguments.Elements[0].StringValue =
			TEXT("/Temp/AssetDocumentAI/NoSuchBlackboard.NoSuchBlackboard");
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Unavailable, Handles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback unavailable target failure envelope"), Outcome,
			Before, *Blackboard, Transaction,
			TEXT("AIProvider.Blackboard.EntryReadbackUnavailable"));
	}

	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeEntryReadbackInvocation(*NonBlackboard, Transaction.GetMutationToken(), TEXT("OwnedLocal")),
			Handles, nullptr);
		Passed &= RequireReadbackFailure(*this, TEXT("readback non-Blackboard failure envelope"), Outcome,
			Before, *Blackboard, Transaction,
			TEXT("AIProvider.Blackboard.EntryReadbackUnavailable"));
	}

	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2UpdateExistingTest,
	"UEAssetDoc.AIProvider.BlackboardV2.UpdateExisting.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2UpdateExistingTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: the private prerequisite updates one same-class
	//   owned-local entry through the verified AI mutable-asset lifecycle.
	// - Candidate technique: lifecycle state transition.
	// - Risk or gap: a property-only update can appear successful while omitting
	//   entry metadata or bypassing the transaction-owned lifecycle.
	// - Test: a complete same-class state transition.
	// - Expected observable outcome: all requested authored fields change
	//   together and the operation returns success.
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2UpdateExisting_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("update-existing Blackboard fixture exists"), Blackboard)) return false;
	AddExistingKey(*Blackboard, TEXT("ExistingKey"), 7);
	Blackboard->UpdateParentKeys();
	const int32 ExistingIndex = Blackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
		{ return Entry.EntryName == TEXT("ExistingKey"); });
	if (!TestTrue(TEXT("update-existing fixture closes derived Blackboard keys"), ExistingIndex != INDEX_NONE)) return false;
	Blackboard->Keys[ExistingIndex].EntryDescription = TEXT("Before");
	Blackboard->Keys[ExistingIndex].EntryCategory = TEXT("BeforeCategory");
	Blackboard->Keys[ExistingIndex].bInstanceSynced = false;
	Blackboard->GetOutermost()->SetDirtyFlag(false);
	const UBlackboardKeyType* const OriginalKeyType = Blackboard->Keys[ExistingIndex].KeyType;
	TArray<FName> OriginalNames;
	for (const FBlackboardEntry& Entry : Blackboard->Keys) OriginalNames.Add(Entry.EntryName);

	FBlackboardLifecycleTransaction Transaction;
	FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> Editor = ResolveEditorMutableAsset(
		*this, *Blackboard, Transaction.GetMutationToken(), Handles);
	if (!Editor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> AssetHandle = Handles.AddProviderLocalAIResult(Editor.GetValue());
	if (!TestTrue(TEXT("update-existing fixture obtains an AI mutable Asset handle"), AssetHandle.IsSet())) return false;

	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest Request;
	Request.BlackboardPath = Blackboard->GetPathName();
	Request.KeyName = TEXT("ExistingKey");
	Request.ExpectedClassPath = UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass()->GetPathName();
	Request.CanonicalProperties = ObjectValue(
		{TEXT("Int32Value"), TEXT("StringValue")},
		{IntegerValue(41), StringValue(TEXT("Updated property"))});
	Request.Description = TEXT("Updated description");
	Request.Category = TEXT("UpdatedCategory");
	Request.bInstanceSynced = true;

	FBlackboardLifecycleTransaction PublicTransaction;
	FBlackboardLifecycleHandleResolver PublicHandles(PublicTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> PublicEditor = ResolveEditorMutableAsset(
		*this, *Blackboard, PublicTransaction.GetMutationToken(), PublicHandles);
	if (!PublicEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> PublicHandle =
		PublicHandles.AddProviderLocalAIResult(PublicEditor.GetValue());
	if (!TestTrue(TEXT("public update-existing fixture obtains an AI mutable Asset handle"),
		PublicHandle.IsSet())) return false;
	const FAssetDocumentPrimitiveOutcome PublicOutcome =
		CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeUpdateExistingInvocation(PublicHandle.GetValue(), Request),
			PublicHandles, &PublicTransaction);
	bool Passed = TestEqual(TEXT("public update-existing supported entry succeeds"),
		PublicOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	if (PublicOutcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		const FString ActualDiagnostic = PublicOutcome.Diagnostics.IsEmpty()
			? TEXT("<none>") : PublicOutcome.Diagnostics[0].Code;
		AddInfo(FString::Printf(TEXT("public update-existing diagnostic: %s"),
			*ActualDiagnostic));
		Passed &= TestTrue(TEXT("public update-existing failure reaches the absent registration"),
			HasDiagnostic(PublicOutcome, TEXT("GenericProvider.UnknownPrimitive")));
	}
	if (PublicOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		Passed &= TestEqual(TEXT("public update-existing returns the full canonical envelope"),
			PublicOutcome.Value.ObjectKeys,
			TArray<FString>{TEXT("entry"), TEXT("ordered_local_key_names")});
		Passed &= TestTrue(TEXT("public update-existing returns no handles"),
			PublicOutcome.HandleResults.IsEmpty());
		Passed &= TestEqual(TEXT("public update-existing reports four actual changes"),
			PublicOutcome.ChangedSet.Num(), 4);
		Passed &= TestTrue(TEXT("public update-existing rolls back through Atomic"),
			PublicTransaction.Rollback(TEXT("Test.PublicUpdateExistingRED")).bValid);
		Passed &= TestTrue(TEXT("public update-existing rollback restores the complete pre-state"),
			CastChecked<UAssetDocumentAITypedPreimageTestLocalKeyType>(
				Blackboard->Keys[ExistingIndex].KeyType)->Int32Value == 7
			&& Blackboard->Keys[ExistingIndex].EntryDescription == TEXT("Before")
			&& Blackboard->Keys[ExistingIndex].EntryCategory == TEXT("BeforeCategory")
			&& !Blackboard->Keys[ExistingIndex].bInstanceSynced
			&& !Blackboard->GetOutermost()->IsDirty());
	}

	// Test design:
	// - Behavior under test: public same-class update writes an owned Struct KeyType's
	//   complete canonical DefaultValue and returns its exact canonical entry/readback.
	// - Risk or gap: generic leaf writes can reject the admitted Struct DTO before any
	//   lifecycle action, or can appear to succeed while changing the wrong live value.
	// - Minimal oracle: one nested native Struct value with literal desired/readback DTOs.
	// - Expected current failure reason: canonical Struct WRITE authority is unavailable,
	//   so the public primitive failure-closes as AuthorityUnavailable.
	UPackage* StructPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2UpdateExistingStruct_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* StructBlackboard = NewObject<UBlackboardData>(
		StructPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardKeyType_Struct* StructKeyType = NewObject<UBlackboardKeyType_Struct>(
		StructBlackboard, TEXT("StructKeyType"), RF_Transactional);
	if (!TestNotNull(TEXT("public Struct update-existing Blackboard fixture exists"), StructBlackboard)
		|| !TestNotNull(TEXT("public Struct update-existing owned KeyType fixture exists"), StructKeyType)) return false;
	StructKeyType->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	FAssetDocumentAIBlackboardCanonicalStructNested& InitialStructValue =
		StructKeyType->DefaultValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>();
	InitialStructValue.Count = 3;
	InitialStructValue.Label = TEXT("before");
	FBlackboardEntry StructEntry;
	StructEntry.EntryName = TEXT("StructKey");
	StructEntry.EntryDescription = TEXT("Before struct description");
	StructEntry.EntryCategory = TEXT("BeforeStructCategory");
	StructEntry.KeyType = StructKeyType;
	StructBlackboard->Keys.Add(StructEntry);
	StructBlackboard->UpdateParentKeys();
	StructBlackboard->GetOutermost()->SetDirtyFlag(false);
	const int32 StructIndex = StructBlackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
		{ return Entry.EntryName == TEXT("StructKey"); });
	if (!TestTrue(TEXT("public Struct update-existing fixture has exact local key slot"),
		StructIndex != INDEX_NONE)) return false;
	const UBlackboardKeyType* const OriginalStructKeyType = StructBlackboard->Keys[StructIndex].KeyType;
	TArray<FName> OriginalStructNames;
	for (const FBlackboardEntry& Entry : StructBlackboard->Keys) OriginalStructNames.Add(Entry.EntryName);
	const FBlackboardObservation StructBefore(*StructBlackboard);

	const FAssetDocumentRuntimeValueDto DesiredStructDefault = ObjectValue(
		{TEXT("Properties"), TEXT("Struct")},
		{ObjectValue({TEXT("Count"), TEXT("Label")},
			{IntegerValue(41), StringValue(TEXT("desired"))}),
			StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())});
	const FAssetDocumentRuntimeValueDto DesiredStructProperties = ObjectValue(
		{TEXT("DefaultValue")}, {DesiredStructDefault});
	const FAssetDocumentRuntimeValueDto ExpectedStructEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("StructCategory")), StringValue(TEXT("Updated struct description")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			DesiredStructProperties, StringValue(TEXT("StructKey")), BooleanValue(true)});
	const FAssetDocumentRuntimeValueDto ExpectedStructOutput = ObjectValue(
		{TEXT("entry"), TEXT("ordered_local_key_names")},
		{ExpectedStructEntry, ArrayValue({StringValue(TEXT("StructKey"))})});
	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest StructRequest;
	StructRequest.BlackboardPath = StructBlackboard->GetPathName();
	StructRequest.KeyName = TEXT("StructKey");
	StructRequest.ExpectedClassPath = UBlackboardKeyType_Struct::StaticClass()->GetPathName();
	StructRequest.CanonicalProperties = DesiredStructProperties;
	StructRequest.Description = TEXT("Updated struct description");
	StructRequest.Category = TEXT("StructCategory");
	StructRequest.bInstanceSynced = true;
	FBlackboardLifecycleTransaction StructTransaction;
	FBlackboardLifecycleHandleResolver StructHandles(StructTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> StructEditor = ResolveEditorMutableAsset(
		*this, *StructBlackboard, StructTransaction.GetMutationToken(), StructHandles);
	if (!StructEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> StructHandle =
		StructHandles.AddProviderLocalAIResult(StructEditor.GetValue());
	if (!TestTrue(TEXT("public Struct update-existing fixture obtains an AI mutable Asset handle"),
		StructHandle.IsSet())) return false;
	AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ResetPrepareAudit();
	AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ResetOuterPreflightAudit();
	const FAssetDocumentPrimitiveOutcome StructOutcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeUpdateExistingInvocation(StructHandle.GetValue(), StructRequest), StructHandles, &StructTransaction);
	const AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EPrepareAuditTerminal StructPrepareAudit =
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::GetPrepareAuditTerminal();
	AddInfo(FString::Printf(TEXT("public Struct update-existing prepare audit: %s"),
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::GetPrepareAuditTerminalName(StructPrepareAudit)));
	const AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal StructOuterPreflightAudit =
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::GetOuterPreflightAuditTerminal();
	AddInfo(FString::Printf(TEXT("public Struct update-existing outer preflight audit: %s"),
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::GetOuterPreflightAuditTerminalName(
			StructOuterPreflightAudit)));
	bool StructPassed = TestEqual(TEXT("public Struct same-class update-existing succeeds"),
		StructOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	StructPassed &= TestEqual(TEXT("public Struct update-existing reaches prepared canonical scratch"),
		StructPrepareAudit,
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EPrepareAuditTerminal::Prepared);
	StructPassed &= TestEqual(TEXT("public Struct update-existing completes outer preflight"),
		StructOuterPreflightAudit,
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal::Prepared);
	if (StructOutcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		const FString ActualDiagnostic = StructOutcome.Diagnostics.IsEmpty()
			? TEXT("<none>") : StructOutcome.Diagnostics[0].Code;
		AddInfo(FString::Printf(TEXT("public Struct update-existing diagnostic: %s"),
			*ActualDiagnostic));
		StructPassed &= RequireExactFailureTerminal(*this,
			TEXT("public Struct same-class update-existing terminal"), StructOutcome,
			StructBefore, *StructBlackboard, StructTransaction,
			TEXT("AIProvider.AuthorityUnavailable"));
		TArray<FName> ActualStructNames;
		for (const FBlackboardEntry& Entry : StructBlackboard->Keys) ActualStructNames.Add(Entry.EntryName);
		const UBlackboardKeyType_Struct* PreservedStructKeyType = StructIndex < StructBlackboard->Keys.Num()
			? Cast<UBlackboardKeyType_Struct>(StructBlackboard->Keys[StructIndex].KeyType) : nullptr;
		const FAssetDocumentAIBlackboardCanonicalStructNested* PreservedValue =
			PreservedStructKeyType != nullptr
				? PreservedStructKeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>()
				: nullptr;
		StructPassed &= TestTrue(TEXT("public Struct failure preserves pointer/default/order and clean package"),
			PreservedStructKeyType == OriginalStructKeyType
			&& PreservedValue != nullptr && PreservedValue->Count == 3
			&& PreservedValue->Label == TEXT("before")
			&& ActualStructNames == OriginalStructNames && !StructBlackboard->GetOutermost()->IsDirty());
	}
	if (StructOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		if (!RuntimeValueEquals(StructOutcome.Value, ExpectedStructOutput))
		{
			AddInfo(FString::Printf(TEXT("public Struct update-existing DTO first mismatch: %s"),
				*FirstRuntimeValueDifference(StructOutcome.Value, ExpectedStructOutput)));
		}
		const UBlackboardKeyType_Struct* UpdatedStructKeyType = StructIndex < StructBlackboard->Keys.Num()
			? Cast<UBlackboardKeyType_Struct>(StructBlackboard->Keys[StructIndex].KeyType) : nullptr;
		const FAssetDocumentAIBlackboardCanonicalStructNested* UpdatedValue =
			UpdatedStructKeyType != nullptr
				? UpdatedStructKeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>()
				: nullptr;
		StructPassed &= RequireSuccess(*this, TEXT("public Struct same-class update-existing"), StructOutcome)
			&& TestTrue(TEXT("public Struct update-existing returns exact canonical entry/order"),
				RuntimeValueEquals(StructOutcome.Value, ExpectedStructOutput))
			&& TestEqual(TEXT("public Struct update-existing preserves the exact KeyType instance"),
				static_cast<const UBlackboardKeyType*>(UpdatedStructKeyType), OriginalStructKeyType)
			&& TestTrue(TEXT("public Struct update-existing writes exact live InstancedStruct"),
				UpdatedValue != nullptr && UpdatedValue->Count == 41 && UpdatedValue->Label == TEXT("desired"));
		FBlackboardLifecycleTransaction StructReadbackTransaction;
		FBlackboardLifecycleHandleResolver StructReadbackHandles(StructReadbackTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome StructReadback = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryReadbackInvocation(*StructBlackboard, StructReadbackTransaction.GetMutationToken(),
				TEXT("StructKey")), StructReadbackHandles, nullptr);
		StructPassed &= RequireSuccess(*this, TEXT("public Struct update-existing readback"), StructReadback)
			&& TestTrue(TEXT("public Struct update-existing readback returns exact canonical entry"),
				RuntimeValueEquals(StructReadback.Value,
					ObjectValue({TEXT("entry")}, {ExpectedStructEntry})));
	}
	Passed &= StructPassed;

	// Test design:
	// - Behavior under test: an admitted same-class Struct update transitions one
	//   valid native FInstancedStruct DefaultValue to the canonical Null state.
	// - Candidate technique: finite-algebra state transition (valid -> Null).
	// - Risk or gap: the live write path can prepare Null but reject its invalid
	//   scratch at apply time, leaving the public UpdateExisting row incomplete.
	// - Test: a changed Null DefaultValue together with changed metadata.
	// - Expected observable outcome: complete public entry/order evidence, the
	//   same KeyType instance, an invalid live DefaultValue, and typed rollback.
	UPackage* NullStructPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2UpdateExistingStructNull_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* NullStructBlackboard = NewObject<UBlackboardData>(
		NullStructPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardKeyType_Struct* NullStructKeyType = NewObject<UBlackboardKeyType_Struct>(
		NullStructBlackboard, TEXT("StructKeyType"), RF_Transactional);
	if (!TestNotNull(TEXT("SC-WR Update Null Blackboard fixture exists"), NullStructBlackboard)
		|| !TestNotNull(TEXT("SC-WR Update Null owned KeyType fixture exists"), NullStructKeyType)) return false;
	NullStructKeyType->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	FAssetDocumentAIBlackboardCanonicalStructNested& NullInitialValue =
		NullStructKeyType->DefaultValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>();
	NullInitialValue.Count = 17;
	NullInitialValue.Label = TEXT("null-before");
	FBlackboardEntry NullStructEntry;
	NullStructEntry.EntryName = TEXT("NullStructKey");
	NullStructEntry.EntryDescription = TEXT("Before null struct description");
	NullStructEntry.EntryCategory = TEXT("BeforeNullStructCategory");
	NullStructEntry.KeyType = NullStructKeyType;
	NullStructBlackboard->Keys.Add(NullStructEntry);
	NullStructBlackboard->UpdateParentKeys();
	NullStructBlackboard->GetOutermost()->SetDirtyFlag(false);
	const int32 NullStructIndex = NullStructBlackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
		{ return Entry.EntryName == TEXT("NullStructKey"); });
	if (!TestTrue(TEXT("SC-WR Update Null fixture has exact local key slot"),
		NullStructIndex != INDEX_NONE)) return false;
	const UBlackboardKeyType* const OriginalNullStructKeyType = NullStructBlackboard->Keys[NullStructIndex].KeyType;
	TArray<FName> OriginalNullStructNames;
	for (const FBlackboardEntry& Entry : NullStructBlackboard->Keys) OriginalNullStructNames.Add(Entry.EntryName);

	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest NullStructRequest;
	NullStructRequest.BlackboardPath = NullStructBlackboard->GetPathName();
	NullStructRequest.KeyName = TEXT("NullStructKey");
	NullStructRequest.ExpectedClassPath = UBlackboardKeyType_Struct::StaticClass()->GetPathName();
	NullStructRequest.CanonicalProperties = ObjectValue(
		{TEXT("DefaultValue")}, {NullValue()});
	NullStructRequest.Description = TEXT("Updated null struct description");
	NullStructRequest.Category = TEXT("UpdatedNullStructCategory");
	NullStructRequest.bInstanceSynced = true;
	const FAssetDocumentRuntimeValueDto ExpectedNullStructEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("UpdatedNullStructCategory")), StringValue(TEXT("Updated null struct description")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {NullValue()}),
			StringValue(TEXT("NullStructKey")), BooleanValue(true)});

	FBlackboardLifecycleTransaction NullStructTransaction;
	FBlackboardLifecycleHandleResolver NullStructHandles(NullStructTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> NullStructEditor = ResolveEditorMutableAsset(
		*this, *NullStructBlackboard, NullStructTransaction.GetMutationToken(), NullStructHandles);
	if (!NullStructEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> NullStructHandle =
		NullStructHandles.AddProviderLocalAIResult(NullStructEditor.GetValue());
	if (!TestTrue(TEXT("SC-WR Update Null fixture obtains an AI mutable Asset handle"),
		NullStructHandle.IsSet())) return false;
	const FAssetDocumentPrimitiveOutcome NullStructOutcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeUpdateExistingInvocation(NullStructHandle.GetValue(), NullStructRequest),
		NullStructHandles, &NullStructTransaction);
	bool NullStructPassed = RequireSuccess(*this, TEXT("SC-WR UpdateExisting valid Struct to Null"), NullStructOutcome);
	if (NullStructOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		const UBlackboardKeyType_Struct* UpdatedNullStructKeyType = NullStructIndex < NullStructBlackboard->Keys.Num()
			? Cast<UBlackboardKeyType_Struct>(NullStructBlackboard->Keys[NullStructIndex].KeyType) : nullptr;
		NullStructPassed &= TestTrue(TEXT("SC-WR Update Null returns complete authored entry/order"),
			RuntimeValueEquals(NullStructOutcome.Value, ObjectValue({TEXT("entry"), TEXT("ordered_local_key_names")},
				{ExpectedNullStructEntry, ArrayValue({StringValue(TEXT("NullStructKey"))})})))
			&& TestEqual(TEXT("SC-WR Update Null preserves exact KeyType identity"),
				static_cast<const UBlackboardKeyType*>(UpdatedNullStructKeyType), OriginalNullStructKeyType)
			&& TestTrue(TEXT("SC-WR Update Null writes invalid live DefaultValue"),
				UpdatedNullStructKeyType != nullptr && !UpdatedNullStructKeyType->DefaultValue.IsValid())
			&& TestEqual(TEXT("SC-WR Update Null reports four actual member changes"),
				NullStructOutcome.ChangedSet.Num(), 4)
			&& TestEqual(TEXT("SC-WR Update Null captures metadata then key-type typed preimages"),
				NullStructTransaction.Order, TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint")})
			&& TestEqual(TEXT("SC-WR Update Null enlists one two-item typed footprint"),
				NullStructTransaction.EnlistedFootprint.Num(), 2);
		FBlackboardLifecycleTransaction NullStructReadbackTransaction;
		FBlackboardLifecycleHandleResolver NullStructReadbackHandles(NullStructReadbackTransaction.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome NullStructReadback = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryReadbackInvocation(*NullStructBlackboard, NullStructReadbackTransaction.GetMutationToken(),
				TEXT("NullStructKey")), NullStructReadbackHandles, nullptr);
		NullStructPassed &= RequireSuccess(*this, TEXT("SC-WR Update Null public readback"), NullStructReadback)
			&& TestTrue(TEXT("SC-WR Update Null public readback emits canonical Null DefaultValue"),
				RuntimeValueEquals(NullStructReadback.Value, ObjectValue({TEXT("entry")}, {ExpectedNullStructEntry})));
		if (NullStructTransaction.EnlistedFootprint.Num() == 2)
		{
			NullStructPassed &= TestEqual(TEXT("SC-WR Update Null footprint starts with metadata"),
				NullStructTransaction.EnlistedFootprint[0].RollbackComponentId,
				FString(TEXT("assetdocument.ai.blackboard.entry_metadata.v2")))
				&& TestEqual(TEXT("SC-WR Update Null footprint ends with key-type properties"),
					NullStructTransaction.EnlistedFootprint[1].RollbackComponentId,
					FString(TEXT("assetdocument.ai.blackboard.key_type_properties.v2")));
		}
		NullStructPassed &= TestTrue(TEXT("SC-WR Update Null rollback succeeds"),
			NullStructTransaction.Rollback(TEXT("Test.StructNullRollback")).bValid);
		const UBlackboardKeyType_Struct* RolledBackNullStructKeyType = NullStructIndex < NullStructBlackboard->Keys.Num()
			? Cast<UBlackboardKeyType_Struct>(NullStructBlackboard->Keys[NullStructIndex].KeyType) : nullptr;
		const FAssetDocumentAIBlackboardCanonicalStructNested* RolledBackNullValue =
			RolledBackNullStructKeyType != nullptr
				? RolledBackNullStructKeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>() : nullptr;
		TArray<FName> RolledBackNullStructNames;
		for (const FBlackboardEntry& Entry : NullStructBlackboard->Keys) RolledBackNullStructNames.Add(Entry.EntryName);
		NullStructPassed &= TestTrue(TEXT("SC-WR Update Null rollback restores valid Struct metadata order and dirty state"),
			RolledBackNullStructKeyType == OriginalNullStructKeyType
			&& RolledBackNullValue != nullptr && RolledBackNullValue->Count == 17
			&& RolledBackNullValue->Label == TEXT("null-before")
			&& NullStructBlackboard->Keys[NullStructIndex].EntryDescription == TEXT("Before null struct description")
			&& NullStructBlackboard->Keys[NullStructIndex].EntryCategory == TEXT("BeforeNullStructCategory")
			&& !NullStructBlackboard->Keys[NullStructIndex].bInstanceSynced
			&& RolledBackNullStructNames == OriginalNullStructNames && !NullStructBlackboard->GetOutermost()->IsDirty());
	}
	Passed &= NullStructPassed;

	// Test design:
	// - Behavior under test: an exact same-class Struct update is a no-op when
	//   native DefaultValue, metadata, and local order already equal the literal DTO.
	// - Risk or gap: a canonical Struct no-op can still capture a transaction or
	//   emit a lifecycle change while returning an apparently complete response.
	// - Minimal oracle: one nested native Struct fixture and a hand-authored
	//   complete entry/order response with zero actions, footprint, and changes.
	UPackage* NoOpStructPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2UpdateExistingStructNoOp_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* NoOpStructBlackboard = NewObject<UBlackboardData>(
		NoOpStructPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardKeyType_Struct* NoOpStructKeyType = NewObject<UBlackboardKeyType_Struct>(
		NoOpStructBlackboard, TEXT("StructKeyType"), RF_Transactional);
	if (!TestNotNull(TEXT("SC-WR Update Struct no-op Blackboard fixture exists"), NoOpStructBlackboard)
		|| !TestNotNull(TEXT("SC-WR Update Struct no-op owned KeyType fixture exists"), NoOpStructKeyType)) return false;
	NoOpStructKeyType->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	FAssetDocumentAIBlackboardCanonicalStructNested& NoOpInitialValue =
		NoOpStructKeyType->DefaultValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>();
	NoOpInitialValue.Count = 29;
	NoOpInitialValue.Label = TEXT("already-canonical");
	FBlackboardEntry NoOpStructEntry;
	NoOpStructEntry.EntryName = TEXT("NoOpStructKey");
	NoOpStructEntry.EntryDescription = TEXT("No-op struct description");
	NoOpStructEntry.EntryCategory = TEXT("NoOpStructCategory");
	NoOpStructEntry.KeyType = NoOpStructKeyType;
	NoOpStructBlackboard->Keys.Add(NoOpStructEntry);
	NoOpStructBlackboard->UpdateParentKeys();
	NoOpStructBlackboard->GetOutermost()->SetDirtyFlag(false);
	const int32 NoOpStructIndex = NoOpStructBlackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
		{ return Entry.EntryName == TEXT("NoOpStructKey"); });
	if (!TestTrue(TEXT("SC-WR Update Struct no-op fixture has exact local key slot"),
		NoOpStructIndex != INDEX_NONE)) return false;
	const UBlackboardKeyType* const OriginalNoOpStructKeyType =
		NoOpStructBlackboard->Keys[NoOpStructIndex].KeyType;
	TArray<FName> OriginalNoOpStructNames;
	for (const FBlackboardEntry& Entry : NoOpStructBlackboard->Keys) OriginalNoOpStructNames.Add(Entry.EntryName);
	const FAssetDocumentRuntimeValueDto NoOpStructDefault = ObjectValue(
		{TEXT("Properties"), TEXT("Struct")},
		{ObjectValue({TEXT("Count"), TEXT("Label")},
			{IntegerValue(29), StringValue(TEXT("already-canonical"))}),
			StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())});
	const FAssetDocumentRuntimeValueDto NoOpStructProperties = ObjectValue(
		{TEXT("DefaultValue")}, {NoOpStructDefault});
	const FAssetDocumentRuntimeValueDto ExpectedNoOpStructEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("NoOpStructCategory")), StringValue(TEXT("No-op struct description")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			NoOpStructProperties, StringValue(TEXT("NoOpStructKey")), BooleanValue(false)});
	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest NoOpStructRequest;
	NoOpStructRequest.BlackboardPath = NoOpStructBlackboard->GetPathName();
	NoOpStructRequest.KeyName = TEXT("NoOpStructKey");
	NoOpStructRequest.ExpectedClassPath = UBlackboardKeyType_Struct::StaticClass()->GetPathName();
	NoOpStructRequest.CanonicalProperties = NoOpStructProperties;
	NoOpStructRequest.Description = TEXT("No-op struct description");
	NoOpStructRequest.Category = TEXT("NoOpStructCategory");
	NoOpStructRequest.bInstanceSynced = false;
	FBlackboardLifecycleTransaction NoOpStructTransaction;
	FBlackboardLifecycleHandleResolver NoOpStructHandles(NoOpStructTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> NoOpStructEditor = ResolveEditorMutableAsset(
		*this, *NoOpStructBlackboard, NoOpStructTransaction.GetMutationToken(), NoOpStructHandles);
	if (!NoOpStructEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> NoOpStructHandle =
		NoOpStructHandles.AddProviderLocalAIResult(NoOpStructEditor.GetValue());
	if (!TestTrue(TEXT("SC-WR Update Struct no-op fixture obtains an AI mutable Asset handle"),
		NoOpStructHandle.IsSet())) return false;
	const FAssetDocumentPrimitiveOutcome NoOpStructOutcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeUpdateExistingInvocation(NoOpStructHandle.GetValue(), NoOpStructRequest),
		NoOpStructHandles, &NoOpStructTransaction);
	const UBlackboardKeyType_Struct* NoOpResultKeyType = NoOpStructIndex < NoOpStructBlackboard->Keys.Num()
		? Cast<UBlackboardKeyType_Struct>(NoOpStructBlackboard->Keys[NoOpStructIndex].KeyType) : nullptr;
	const FAssetDocumentAIBlackboardCanonicalStructNested* NoOpResultValue = NoOpResultKeyType != nullptr
		? NoOpResultKeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>() : nullptr;
	TArray<FName> ActualNoOpStructNames;
	for (const FBlackboardEntry& Entry : NoOpStructBlackboard->Keys) ActualNoOpStructNames.Add(Entry.EntryName);
	Passed &= RequireSuccess(*this, TEXT("SC-WR UpdateExisting exact Struct no-op"), NoOpStructOutcome)
		&& TestTrue(TEXT("SC-WR Update Struct no-op returns exact complete entry/order"),
			RuntimeValueEquals(NoOpStructOutcome.Value, ObjectValue(
				{TEXT("entry"), TEXT("ordered_local_key_names")},
				{ExpectedNoOpStructEntry, ArrayValue({StringValue(TEXT("NoOpStructKey"))})})))
		&& TestTrue(TEXT("SC-WR Update Struct no-op preserves exact native state and clean package"),
			NoOpResultKeyType == OriginalNoOpStructKeyType
			&& NoOpResultValue != nullptr && NoOpResultValue->Count == 29
			&& NoOpResultValue->Label == TEXT("already-canonical")
			&& NoOpStructBlackboard->Keys[NoOpStructIndex].EntryDescription == TEXT("No-op struct description")
			&& NoOpStructBlackboard->Keys[NoOpStructIndex].EntryCategory == TEXT("NoOpStructCategory")
			&& !NoOpStructBlackboard->Keys[NoOpStructIndex].bInstanceSynced
			&& ActualNoOpStructNames == OriginalNoOpStructNames
			&& !NoOpStructBlackboard->GetOutermost()->IsDirty())
		&& TestTrue(TEXT("SC-WR Update Struct no-op returns no handles or changed members"),
			NoOpStructOutcome.HandleResults.IsEmpty() && NoOpStructOutcome.ChangedSet.IsEmpty())
		&& TestEqual(TEXT("SC-WR Update Struct no-op captures zero actions"),
			NoOpStructTransaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("SC-WR Update Struct no-op enlists zero footprint"),
			NoOpStructTransaction.FootprintEnlistCount, 0);

	AssetDocumentAIBlackboardMutation::BeginBlackboardMetadataGeneratorAudit();
	const FAssetDocumentPrimitiveOutcome Outcome =
		AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
			MakeUpdateExistingPrerequisiteInvocation(AssetHandle.GetValue()), Request, Handles, &Transaction);
	const TArray<FString> GeneratorAudit =
		AssetDocumentAIBlackboardMutation::EndBlackboardMetadataGeneratorAudit();
	const int32 UpdatedIndex = Blackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
		{ return Entry.EntryName == TEXT("ExistingKey"); });
	const UAssetDocumentAITypedPreimageTestLocalKeyType* const KeyType = UpdatedIndex != INDEX_NONE
		? Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[UpdatedIndex].KeyType) : nullptr;
	TArray<FName> UpdatedNames;
	for (const FBlackboardEntry& Entry : Blackboard->Keys) UpdatedNames.Add(Entry.EntryName);
	Passed &= TestEqual(TEXT("update-existing private lifecycle succeeds"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		&& TestTrue(TEXT("update-existing preserves the complete local key order"), UpdatedNames == OriginalNames)
		&& TestNotNull(TEXT("update-existing preserves the live same-class KeyType instance"), KeyType)
		&& TestEqual(TEXT("update-existing preserves the exact KeyType identity"),
			static_cast<const UBlackboardKeyType*>(KeyType), OriginalKeyType)
		&& TestEqual(TEXT("update-existing writes complete KeyType state"), KeyType->Int32Value, 41)
		&& TestEqual(TEXT("update-existing writes entry description"), Blackboard->Keys[UpdatedIndex].EntryDescription,
			FString(TEXT("Updated description")))
		&& TestEqual(TEXT("update-existing writes entry category"), Blackboard->Keys[UpdatedIndex].EntryCategory,
			FName(TEXT("UpdatedCategory")))
		&& TestTrue(TEXT("update-existing writes entry instance-sync state"),
			static_cast<bool>(Blackboard->Keys[UpdatedIndex].bInstanceSynced));
	Passed &= TestEqual(TEXT("update-existing enlists the two capture actions before one footprint"),
		Transaction.Order, TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint")});
	Passed &= TestEqual(TEXT("update-existing enlists exactly one two-item footprint"),
		Transaction.EnlistedFootprint.Num(), 2);
	if (Transaction.EnlistedFootprint.Num() == 2)
	{
		Passed &= TestEqual(TEXT("update-existing footprint starts with metadata aggregate"),
			Transaction.EnlistedFootprint[0].RollbackComponentId,
			FString(TEXT("assetdocument.ai.blackboard.entry_metadata.v2")));
		Passed &= TestEqual(TEXT("update-existing footprint ends with key-type properties"),
			Transaction.EnlistedFootprint[1].RollbackComponentId,
			FString(TEXT("assetdocument.ai.blackboard.key_type_properties.v2")));
	}
	Passed &= TestEqual(TEXT("update-existing reports only actual member changes"), Outcome.ChangedSet.Num(), 4);
	Passed &= TestEqual(TEXT("update-existing uses exactly Blackboard then KeyType allowlisted generators"),
		GeneratorAudit, TArray<FString>{TEXT("Blackboard|Keys"), TEXT("KeyType|Int32Value,StringValue")});

	Blackboard->GetOutermost()->SetDirtyFlag(false);
	FBlackboardLifecycleTransaction NoOpTransaction;
	FBlackboardLifecycleHandleResolver NoOpHandles(NoOpTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> NoOpEditor = ResolveEditorMutableAsset(
		*this, *Blackboard, NoOpTransaction.GetMutationToken(), NoOpHandles);
	if (!NoOpEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> NoOpHandle =
		NoOpHandles.AddProviderLocalAIResult(NoOpEditor.GetValue());
	if (!TestTrue(TEXT("update-existing no-op fixture obtains an AI mutable Asset handle"), NoOpHandle.IsSet())) return false;
	const FAssetDocumentPrimitiveOutcome NoOp =
		AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
			MakeUpdateExistingPrerequisiteInvocation(NoOpHandle.GetValue()), Request, NoOpHandles, &NoOpTransaction);
	Passed &= TestEqual(TEXT("update-existing exact no-op succeeds"), NoOp.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestEqual(TEXT("update-existing exact no-op captures no action"), NoOpTransaction.ActionEnlistCount, 0);
	Passed &= TestEqual(TEXT("update-existing exact no-op enlists no footprint"), NoOpTransaction.FootprintEnlistCount, 0);
	Passed &= TestTrue(TEXT("update-existing exact no-op leaves ChangedSet empty"), NoOp.ChangedSet.IsEmpty());
	Passed &= TestFalse(TEXT("update-existing exact no-op leaves package clean"), Blackboard->GetOutermost()->IsDirty());

	FBlackboardLifecycleTransaction PublicNoOpTransaction;
	FBlackboardLifecycleHandleResolver PublicNoOpHandles(PublicNoOpTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> PublicNoOpEditor = ResolveEditorMutableAsset(
		*this, *Blackboard, PublicNoOpTransaction.GetMutationToken(), PublicNoOpHandles);
	if (!PublicNoOpEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> PublicNoOpHandle =
		PublicNoOpHandles.AddProviderLocalAIResult(PublicNoOpEditor.GetValue());
	if (!PublicNoOpHandle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome PublicNoOp =
		CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeUpdateExistingInvocation(PublicNoOpHandle.GetValue(), Request),
			PublicNoOpHandles, &PublicNoOpTransaction);
	Passed &= TestEqual(TEXT("public update-existing exact no-op succeeds"),
		PublicNoOp.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestEqual(TEXT("public update-existing exact no-op captures no action"),
		PublicNoOpTransaction.ActionEnlistCount, 0);
	Passed &= TestEqual(TEXT("public update-existing exact no-op enlists no footprint"),
		PublicNoOpTransaction.FootprintEnlistCount, 0);
	Passed &= TestTrue(TEXT("public update-existing exact no-op returns full readback without changes"),
		PublicNoOp.Value.ObjectKeys == TArray<FString>{TEXT("entry"), TEXT("ordered_local_key_names")}
			&& PublicNoOp.HandleResults.IsEmpty() && PublicNoOp.ChangedSet.IsEmpty());
	Passed &= TestFalse(TEXT("public update-existing exact no-op leaves package clean"),
		Blackboard->GetOutermost()->IsDirty());

	UBlackboardKeyType_Object* ObjectCodecKey = NewObject<UBlackboardKeyType_Object>(Blackboard);
	UBlackboardKeyType_Class* ClassCodecKey = NewObject<UBlackboardKeyType_Class>(Blackboard);
	UBlackboardKeyType_Enum* EnumCodecKey = NewObject<UBlackboardKeyType_Enum>(Blackboard);
	UEnum* TestEnum = StaticEnum<EAssetDocumentAITypedPreimageTestEnum>();
	if (!TestNotNull(TEXT("update-existing Object codec fixture exists"), ObjectCodecKey)
		|| !TestNotNull(TEXT("update-existing Class codec fixture exists"), ClassCodecKey)
		|| !TestNotNull(TEXT("update-existing Enum codec fixture exists"), EnumCodecKey)
		|| !TestNotNull(TEXT("update-existing Enum type fixture exists"), TestEnum)) return false;
	ObjectCodecKey->BaseClass = UBlackboardData::StaticClass();
	ObjectCodecKey->DefaultValue = nullptr;
	ClassCodecKey->BaseClass = AActor::StaticClass();
	ClassCodecKey->DefaultValue = nullptr;
	EnumCodecKey->EnumType = TestEnum;
	EnumCodecKey->EnumName = TestEnum->GetPathName();
	EnumCodecKey->bIsEnumNameValid = true;
	EnumCodecKey->DefaultValue = static_cast<uint8>(EAssetDocumentAITypedPreimageTestEnum::Original);
	for (const TPair<FName, UBlackboardKeyType*>& Entry : {
		TPair<FName, UBlackboardKeyType*>{TEXT("ObjectCodec"), ObjectCodecKey},
		TPair<FName, UBlackboardKeyType*>{TEXT("ClassCodec"), ClassCodecKey},
		TPair<FName, UBlackboardKeyType*>{TEXT("EnumCodec"), EnumCodecKey}})
	{
		FBlackboardEntry NewEntry;
		NewEntry.EntryName = Entry.Key;
		NewEntry.KeyType = Entry.Value;
		Blackboard->Keys.Add(NewEntry);
	}
	Blackboard->UpdateParentKeys();
	auto RunStockCodec = [&](const TCHAR* Label, UBlackboardKeyType& KeyType,
		const FString& Name, const FAssetDocumentRuntimeValueDto& Properties)
	{
		FBlackboardLifecycleTransaction CodecTransaction;
		FBlackboardLifecycleHandleResolver CodecHandles(CodecTransaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> CodecEditor = ResolveEditorMutableAsset(
			*this, *Blackboard, CodecTransaction.GetMutationToken(), CodecHandles);
		if (!CodecEditor.IsSet()) return false;
		const TOptional<FAssetDocumentOpaqueHandle> CodecHandle =
			CodecHandles.AddProviderLocalAIResult(CodecEditor.GetValue());
		if (!CodecHandle.IsSet()) return false;
		AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest CodecRequest;
		CodecRequest.BlackboardPath = Blackboard->GetPathName();
		CodecRequest.KeyName = Name;
		CodecRequest.ExpectedClassPath = KeyType.GetClass()->GetPathName();
		CodecRequest.CanonicalProperties = Properties;
		CodecRequest.Description = FString::Printf(TEXT("%s description"), Label);
		CodecRequest.Category = FString::Printf(TEXT("%s category"), Label);
		CodecRequest.bInstanceSynced = true;
		const FAssetDocumentPrimitiveOutcome CodecOutcome =
			AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
				MakeUpdateExistingPrerequisiteInvocation(CodecHandle.GetValue()), CodecRequest,
				CodecHandles, &CodecTransaction);
		const FAssetDocumentRuntimeValueDto* ActualProperties =
			CodecOutcome.Value.Elements.IsValidIndex(0) && CodecOutcome.Value.Elements[0].Elements.IsValidIndex(3)
				? &CodecOutcome.Value.Elements[0].Elements[3] : nullptr;
		return TestEqual(*FString::Printf(TEXT("%s typed codec succeeds"), Label), CodecOutcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
			&& TestNotNull(*FString::Printf(TEXT("%s typed codec returns canonical properties"), Label), ActualProperties)
			&& (ActualProperties == nullptr || TestTrue(*FString::Printf(TEXT("%s typed codec round-trips exactly"), Label),
				RuntimeValueEquals(*ActualProperties, Properties)));
	};
	Passed &= RunStockCodec(TEXT("update-existing Object"), *ObjectCodecKey, TEXT("ObjectCodec"),
		ObjectValue({TEXT("BaseClass"), TEXT("DefaultValue")},
			{ReferenceValue(TEXT("ClassRef"), *UBlackboardData::StaticClass()), ReferenceValue(TEXT("AssetRef"), *Blackboard)}));
	Passed &= RunStockCodec(TEXT("update-existing Class"), *ClassCodecKey, TEXT("ClassCodec"),
		ObjectValue({TEXT("BaseClass"), TEXT("DefaultValue")},
			{ReferenceValue(TEXT("ClassRef"), *AActor::StaticClass()), ReferenceValue(TEXT("ClassRef"), *APawn::StaticClass())}));
	Passed &= RunStockCodec(TEXT("update-existing Enum"), *EnumCodecKey, TEXT("EnumCodec"),
		ObjectValue({TEXT("DefaultValue"), TEXT("EnumName"), TEXT("EnumType")},
			{StringValue(TEXT("Replacement")), StringValue(TestEnum->GetPathName()), ReferenceValue(TEXT("AssetRef"), *TestEnum)}));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2UpdateExistingFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.UpdateExisting.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2UpdateExistingFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2UpdateExistingFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Parent = NewObject<UBlackboardData>(
		Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("update-existing failure Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("update-existing failure parent fixture exists"), Parent)) return false;
	AddExistingKey(*Blackboard, TEXT("ExistingKey"), 7);
	AddExistingKey(*Parent, TEXT("InheritedOnly"), 19);
	Parent->UpdateParentKeys();
	Blackboard->Parent = Parent;
	Blackboard->UpdateParentKeys();
	FBlackboardLifecycleTransaction Transaction;
	FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> Editor = ResolveEditorMutableAsset(
		*this, *Blackboard, Transaction.GetMutationToken(), Handles);
	if (!Editor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> AssetHandle = Handles.AddProviderLocalAIResult(Editor.GetValue());
	if (!TestTrue(TEXT("update-existing failure fixture obtains an AI mutable Asset handle"), AssetHandle.IsSet())) return false;

	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest Request;
	Request.BlackboardPath = Blackboard->GetPathName();
	Request.KeyName = TEXT("MissingKey");
	Request.ExpectedClassPath = UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass()->GetPathName();
	Request.CanonicalProperties = ObjectValue({TEXT("Int32Value"), TEXT("StringValue")},
		{IntegerValue(41), StringValue(TEXT("Updated property"))});
	Request.Description = TEXT("Updated description");
	Request.Category = TEXT("UpdatedCategory");
	Request.bInstanceSynced = true;
	const TSharedRef<IAssetDocumentPrimitiveProvider> PublicProvider =
		CreateAssetDocumentAIPrimitiveProvider();
	auto RunPublicPreflightFailure = [&](const TCHAR* Label,
		const AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& CandidateRequest,
		const TCHAR* ExpectedCode)
	{
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		FBlackboardLifecycleTransaction CandidateTransaction;
		FBlackboardLifecycleHandleResolver CandidateHandles(CandidateTransaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> CandidateEditor = ResolveEditorMutableAsset(
			*this, *Blackboard, CandidateTransaction.GetMutationToken(), CandidateHandles);
		if (!CandidateEditor.IsSet()) return false;
		const TOptional<FAssetDocumentOpaqueHandle> CandidateHandle =
			CandidateHandles.AddProviderLocalAIResult(CandidateEditor.GetValue());
		if (!CandidateHandle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome CandidateOutcome = PublicProvider->Invoke(
			MakeUpdateExistingInvocation(CandidateHandle.GetValue(), CandidateRequest),
			CandidateHandles, &CandidateTransaction);
		return TestEqual(*FString::Printf(TEXT("%s fails closed"), Label),
			CandidateOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed)
			&& TestTrue(*FString::Printf(TEXT("%s uses its exact public terminal"), Label),
				HasDiagnostic(CandidateOutcome, ExpectedCode))
			&& TestTrue(*FString::Printf(TEXT("%s returns empty public output"), Label),
				CandidateOutcome.Value.ObjectKeys.IsEmpty()
					&& CandidateOutcome.Value.Elements.IsEmpty()
					&& CandidateOutcome.HandleResults.IsEmpty()
					&& CandidateOutcome.ChangedSet.IsEmpty())
			&& TestEqual(*FString::Printf(TEXT("%s captures no action"), Label),
				CandidateTransaction.ActionEnlistCount, 0)
			&& TestEqual(*FString::Printf(TEXT("%s enlists no footprint"), Label),
				CandidateTransaction.FootprintEnlistCount, 0)
			&& TestFalse(*FString::Printf(TEXT("%s leaves the package clean"), Label),
				Blackboard->GetOutermost()->IsDirty());
	};

	// Test design:
	// - Behavior under test: a syntactically valid public Struct update rejects a
	//   malformed nested admitted DTO at canonical WRITE preflight.
	// - Risk or gap: a malformed nested value can be mistaken for an invocation
	//   shape error or reach transaction capture before the finite algebra refuses it.
	// - Minimal oracle: the known nested Struct path with a String where Count
	//   requires an Integer, with exact no-residue and preflight-audit assertions.
	UPackage* MalformedStructPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2UpdateExistingMalformedStruct_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* MalformedStructBlackboard = NewObject<UBlackboardData>(
		MalformedStructPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardKeyType_Struct* MalformedStructKeyType = NewObject<UBlackboardKeyType_Struct>(
		MalformedStructBlackboard, TEXT("StructKeyType"), RF_Transactional);
	if (!TestNotNull(TEXT("SC-WR malformed Struct Blackboard fixture exists"), MalformedStructBlackboard)
		|| !TestNotNull(TEXT("SC-WR malformed Struct owned KeyType fixture exists"), MalformedStructKeyType)) return false;
	MalformedStructKeyType->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	FAssetDocumentAIBlackboardCanonicalStructNested& MalformedInitialValue =
		MalformedStructKeyType->DefaultValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>();
	MalformedInitialValue.Count = 13;
	MalformedInitialValue.Label = TEXT("malformed-before");
	FBlackboardEntry MalformedStructEntry;
	MalformedStructEntry.EntryName = TEXT("MalformedStructKey");
	MalformedStructEntry.EntryDescription = TEXT("Malformed struct description");
	MalformedStructEntry.EntryCategory = TEXT("MalformedStructCategory");
	MalformedStructEntry.KeyType = MalformedStructKeyType;
	MalformedStructBlackboard->Keys.Add(MalformedStructEntry);
	MalformedStructBlackboard->UpdateParentKeys();
	MalformedStructBlackboard->GetOutermost()->SetDirtyFlag(false);
	const int32 MalformedStructIndex =
		MalformedStructBlackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
			{ return Entry.EntryName == TEXT("MalformedStructKey"); });
	if (!TestTrue(TEXT("SC-WR malformed Struct fixture has exact local key slot"),
		MalformedStructIndex != INDEX_NONE)) return false;
	const UBlackboardKeyType* const OriginalMalformedStructKeyType =
		MalformedStructBlackboard->Keys[MalformedStructIndex].KeyType;
	TArray<FName> OriginalMalformedStructNames;
	for (const FBlackboardEntry& Entry : MalformedStructBlackboard->Keys)
		OriginalMalformedStructNames.Add(Entry.EntryName);
	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest MalformedStructRequest;
	MalformedStructRequest.BlackboardPath = MalformedStructBlackboard->GetPathName();
	MalformedStructRequest.KeyName = TEXT("MalformedStructKey");
	MalformedStructRequest.ExpectedClassPath = UBlackboardKeyType_Struct::StaticClass()->GetPathName();
	MalformedStructRequest.CanonicalProperties = ObjectValue(
		{TEXT("DefaultValue")}, {ObjectValue(
			{TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({TEXT("Count"), TEXT("Label")},
				{StringValue(TEXT("wrong-scalar-kind")), StringValue(TEXT("malformed"))}),
				StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())})});
	MalformedStructRequest.Description = TEXT("Must not write malformed Struct");
	MalformedStructRequest.Category = TEXT("MustNotWriteMalformedStruct");
	MalformedStructRequest.bInstanceSynced = true;
	FBlackboardLifecycleTransaction MalformedStructTransaction;
	FBlackboardLifecycleHandleResolver MalformedStructHandles(MalformedStructTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> MalformedStructEditor = ResolveEditorMutableAsset(
		*this, *MalformedStructBlackboard, MalformedStructTransaction.GetMutationToken(), MalformedStructHandles);
	if (!MalformedStructEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> MalformedStructHandle =
		MalformedStructHandles.AddProviderLocalAIResult(MalformedStructEditor.GetValue());
	if (!TestTrue(TEXT("SC-WR malformed Struct fixture obtains an AI mutable Asset handle"),
		MalformedStructHandle.IsSet())) return false;
	const FBlackboardObservation MalformedStructBefore(*MalformedStructBlackboard);
	AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ResetPrepareAudit();
	AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ResetOuterPreflightAudit();
	const FAssetDocumentPrimitiveOutcome MalformedStructOutcome = PublicProvider->Invoke(
		MakeUpdateExistingInvocation(MalformedStructHandle.GetValue(), MalformedStructRequest),
		MalformedStructHandles, &MalformedStructTransaction);
	const AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EPrepareAuditTerminal MalformedPrepareAudit =
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::GetPrepareAuditTerminal();
	const AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal MalformedOuterPreflightAudit =
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::GetOuterPreflightAuditTerminal();
	const UBlackboardKeyType_Struct* MalformedResultKeyType =
		MalformedStructIndex < MalformedStructBlackboard->Keys.Num()
			? Cast<UBlackboardKeyType_Struct>(MalformedStructBlackboard->Keys[MalformedStructIndex].KeyType) : nullptr;
	const FAssetDocumentAIBlackboardCanonicalStructNested* MalformedResultValue =
		MalformedResultKeyType != nullptr
			? MalformedResultKeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>() : nullptr;
	TArray<FName> ActualMalformedStructNames;
	for (const FBlackboardEntry& Entry : MalformedStructBlackboard->Keys)
		ActualMalformedStructNames.Add(Entry.EntryName);
	const bool MalformedStructPassed = TestEqual(TEXT("SC-WR malformed Struct DTO fails"), MalformedStructOutcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed)
		&& TestEqual(TEXT("SC-WR malformed Struct DTO returns exactly one terminal"),
			MalformedStructOutcome.Diagnostics.Num(), 1)
		&& TestTrue(TEXT("SC-WR malformed Struct DTO uses only EntrySchemaInvalid"),
			MalformedStructOutcome.Diagnostics.Num() == 1
				&& MalformedStructOutcome.Diagnostics[0].Code == TEXT("AIProvider.Blackboard.EntrySchemaInvalid"))
		&& TestTrue(TEXT("SC-WR malformed Struct DTO returns empty output"),
			MalformedStructOutcome.Value.ObjectKeys.IsEmpty()
				&& MalformedStructOutcome.Value.Elements.IsEmpty()
				&& MalformedStructOutcome.HandleResults.IsEmpty()
				&& MalformedStructOutcome.ChangedSet.IsEmpty())
		&& TestEqual(TEXT("SC-WR malformed Struct DTO reaches canonical scratch preflight"),
			MalformedPrepareAudit,
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EPrepareAuditTerminal::ScratchDecodeFailed)
		&& TestEqual(TEXT("SC-WR malformed Struct DTO reaches outer canonical preflight"),
			MalformedOuterPreflightAudit,
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal::CanonicalPrepareFailed)
		&& TestEqual(TEXT("SC-WR malformed Struct DTO captures zero actions"),
			MalformedStructTransaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("SC-WR malformed Struct DTO enlists zero footprint"),
			MalformedStructTransaction.FootprintEnlistCount, 0)
		&& RequireObservation(*this, TEXT("SC-WR malformed Struct DTO preserves derived Blackboard state"),
			MalformedStructBefore, *MalformedStructBlackboard)
		&& TestTrue(TEXT("SC-WR malformed Struct DTO preserves exact native state and clean package"),
			MalformedResultKeyType == OriginalMalformedStructKeyType
			&& MalformedResultValue != nullptr && MalformedResultValue->Count == 13
			&& MalformedResultValue->Label == TEXT("malformed-before")
			&& MalformedStructBlackboard->Keys[MalformedStructIndex].EntryDescription == TEXT("Malformed struct description")
			&& MalformedStructBlackboard->Keys[MalformedStructIndex].EntryCategory == TEXT("MalformedStructCategory")
			&& !MalformedStructBlackboard->Keys[MalformedStructIndex].bInstanceSynced
			&& ActualMalformedStructNames == OriginalMalformedStructNames
			&& !MalformedStructBlackboard->GetOutermost()->IsDirty());

	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest SchemaRequest = Request;
	SchemaRequest.KeyName = TEXT("ExistingKey");
	FBlackboardLifecycleTransaction SchemaTransaction;
	FBlackboardLifecycleHandleResolver SchemaHandles(SchemaTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> SchemaEditor = ResolveEditorMutableAsset(
		*this, *Blackboard, SchemaTransaction.GetMutationToken(), SchemaHandles);
	if (!SchemaEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> SchemaHandle =
		SchemaHandles.AddProviderLocalAIResult(SchemaEditor.GetValue());
	if (!SchemaHandle.IsSet()) return false;
	FAssetDocumentPrimitiveInvocation InvalidEnvelope =
		MakeUpdateExistingPrerequisiteInvocation(SchemaHandle.GetValue());
	InvalidEnvelope.ValueArguments = ObjectValue(
		{TEXT("blackboard_path"), TEXT("key_name")},
		{StringValue(SchemaRequest.BlackboardPath), StringValue(SchemaRequest.KeyName)});
	const FAssetDocumentPrimitiveOutcome InvalidEnvelopeOutcome =
		PublicProvider->Invoke(InvalidEnvelope, SchemaHandles, &SchemaTransaction);
	FAssetDocumentPrimitiveInvocation InvalidEntry =
		MakeUpdateExistingInvocation(SchemaHandle.GetValue(), SchemaRequest);
	InvalidEntry.ValueArguments.Elements[1].Elements[4] = StringValue(TEXT("DifferentName"));
	const FAssetDocumentPrimitiveOutcome InvalidEntryOutcome =
		PublicProvider->Invoke(InvalidEntry, SchemaHandles, &SchemaTransaction);
	bool Passed = TestTrue(TEXT("public update-existing malformed envelope uses InvalidValueShape"),
		HasDiagnostic(InvalidEnvelopeOutcome, TEXT("AIProvider.InvalidValueShape")))
		&& TestTrue(TEXT("public update-existing malformed entry uses EntrySchemaInvalid"),
			HasDiagnostic(InvalidEntryOutcome, TEXT("AIProvider.Blackboard.EntrySchemaInvalid")))
		&& TestTrue(TEXT("public update-existing schema failures return empty output"),
			InvalidEnvelopeOutcome.Value.ObjectKeys.IsEmpty()
				&& InvalidEnvelopeOutcome.HandleResults.IsEmpty()
				&& InvalidEnvelopeOutcome.ChangedSet.IsEmpty()
				&& InvalidEntryOutcome.Value.ObjectKeys.IsEmpty()
				&& InvalidEntryOutcome.HandleResults.IsEmpty()
				&& InvalidEntryOutcome.ChangedSet.IsEmpty())
		&& TestEqual(TEXT("public update-existing schema failures capture no action"),
			SchemaTransaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("public update-existing schema failures enlist no footprint"),
			SchemaTransaction.FootprintEnlistCount, 0);
	Passed &= MalformedStructPassed;
	Passed &= RunPublicPreflightFailure(TEXT("public update-existing true missing key"),
		Request, TEXT("AIProvider.Blackboard.KeyMissing"));
	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest PublicInheritedRequest = Request;
	PublicInheritedRequest.KeyName = TEXT("InheritedOnly");
	Passed &= RunPublicPreflightFailure(TEXT("public update-existing inherited-only key"),
		PublicInheritedRequest, TEXT("AIProvider.Blackboard.KeyInherited"));
	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest PublicClassMismatch = SchemaRequest;
	PublicClassMismatch.ExpectedClassPath = UBlackboardKeyType_Object::StaticClass()->GetPathName();
	Passed &= RunPublicPreflightFailure(TEXT("public update-existing class mismatch"),
		PublicClassMismatch, TEXT("AIProvider.Blackboard.KeyClassMismatch"));

	{
		FBlackboardLifecycleTransaction ContextTransaction;
		FBlackboardLifecycleHandleResolver ContextHandles(ContextTransaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> ContextEditor = ResolveEditorMutableAsset(
			*this, *Blackboard, ContextTransaction.GetMutationToken(), ContextHandles);
		if (!ContextEditor.IsSet()) return false;
		const TOptional<FAssetDocumentOpaqueHandle> ContextHandle =
			ContextHandles.AddProviderLocalAIResult(ContextEditor.GetValue());
		if (!ContextHandle.IsSet()) return false;
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation ContextBefore(*Blackboard);
		FAssetDocumentPrimitiveInvocation WrongPhase =
			MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest);
		WrongPhase.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
		const FAssetDocumentPrimitiveOutcome WrongPhaseOutcome =
			PublicProvider->Invoke(WrongPhase, ContextHandles, &ContextTransaction);
		const FAssetDocumentPrimitiveOutcome NoTransactionOutcome = PublicProvider->Invoke(
			MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest), ContextHandles, nullptr);
		FAssetDocumentPrimitiveInvocation InvalidInvocationTransaction =
			MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest);
		InvalidInvocationTransaction.TransactionId = {};
		const FAssetDocumentPrimitiveOutcome InvalidInvocationTransactionOutcome =
			PublicProvider->Invoke(
				InvalidInvocationTransaction, ContextHandles, &ContextTransaction);

		FBlackboardLifecycleTransaction MismatchedActiveTransaction;
		const FAssetDocumentPrimitiveOutcome MismatchedActiveTransactionOutcome =
			PublicProvider->Invoke(
				MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest),
				ContextHandles, &MismatchedActiveTransaction);

		FBlackboardLifecycleTransaction NonLiveTransaction(
			ContextTransaction.GetMutationToken());
		NonLiveTransaction.MarkReadyToCommit();
		const FAssetDocumentPrimitiveOutcome NonLiveTransactionOutcome =
			PublicProvider->Invoke(
				MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest),
				ContextHandles, &NonLiveTransaction);

		FAssetDocumentMutationToken RequestMismatchToken =
			ContextTransaction.GetMutationToken();
		RequestMismatchToken.RequestId = FGuid::NewGuid();
		FBlackboardLifecycleTransaction RequestMismatchTransaction(
			MoveTemp(RequestMismatchToken));
		const FAssetDocumentPrimitiveOutcome RequestMismatchOutcome =
			PublicProvider->Invoke(
				MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest),
				ContextHandles, &RequestMismatchTransaction);

		FAssetDocumentMutationToken RevisionMismatchToken =
			ContextTransaction.GetMutationToken();
		++RevisionMismatchToken.RuntimeRevision;
		FBlackboardLifecycleTransaction RevisionMismatchTransaction(
			MoveTemp(RevisionMismatchToken));
		const FAssetDocumentPrimitiveOutcome RevisionMismatchOutcome =
			PublicProvider->Invoke(
				MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest),
				ContextHandles, &RevisionMismatchTransaction);

		FAssetDocumentPrimitiveInvocation InvalidHandle =
			MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest);
		InvalidHandle.HandleArguments[0].Handle.Slot += 999;
		const FAssetDocumentPrimitiveOutcome InvalidHandleOutcome =
			PublicProvider->Invoke(InvalidHandle, ContextHandles, &ContextTransaction);
		const bool bPreviousEditor = GIsEditor;
		GIsEditor = false;
		const FAssetDocumentPrimitiveOutcome InvalidContextOutcome = PublicProvider->Invoke(
			MakeUpdateExistingInvocation(ContextHandle.GetValue(), SchemaRequest),
			ContextHandles, &ContextTransaction);
		GIsEditor = bPreviousEditor;
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing disallowed phase"), WrongPhaseOutcome,
			ContextBefore, *Blackboard, ContextTransaction,
			TEXT("AIProvider.InvalidContext"));
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing missing active transaction"), NoTransactionOutcome,
			ContextBefore, *Blackboard, ContextTransaction,
			TEXT("AIProvider.InvalidTransaction"));
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing invalid invocation transaction identity"),
			InvalidInvocationTransactionOutcome, ContextBefore, *Blackboard,
			ContextTransaction, TEXT("AIProvider.InvalidTransaction"));
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing mismatched active transaction"),
			MismatchedActiveTransactionOutcome, ContextBefore, *Blackboard,
			MismatchedActiveTransaction, TEXT("AIProvider.InvalidTransaction"));
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing non-live active transaction"),
			NonLiveTransactionOutcome, ContextBefore, *Blackboard,
			NonLiveTransaction, TEXT("AIProvider.InvalidTransaction"));
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing mutation-token request mismatch"),
			RequestMismatchOutcome, ContextBefore, *Blackboard,
			RequestMismatchTransaction, TEXT("AIProvider.InvalidTransaction"));
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing mutation-token revision mismatch"),
			RevisionMismatchOutcome, ContextBefore, *Blackboard,
			RevisionMismatchTransaction, TEXT("AIProvider.InvalidTransaction"));
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing invalid HA handle"), InvalidHandleOutcome,
			ContextBefore, *Blackboard, ContextTransaction,
			TEXT("AIProvider.InvalidHandle"));
		Passed &= RequireExactFailureTerminal(*this,
			TEXT("public update-existing non-Editor context"), InvalidContextOutcome,
			ContextBefore, *Blackboard, ContextTransaction,
			TEXT("AIProvider.InvalidContext"));
	}
	const int32 MissingPreEntryCount = Blackboard->Keys.Num();
	Blackboard->GetOutermost()->SetDirtyFlag(false);
	const FAssetDocumentPrimitiveOutcome Outcome =
		AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
			MakeUpdateExistingPrerequisiteInvocation(AssetHandle.GetValue()), Request, Handles, &Transaction);
	Passed &= TestEqual(TEXT("update-existing missing local key fails before capture"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed)
		&& TestTrue(TEXT("update-existing true missing key uses KeyMissing"),
			HasDiagnostic(Outcome, TEXT("AIProvider.Blackboard.KeyMissing")))
		&& TestTrue(TEXT("update-existing true missing key returns empty output"),
			Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty()
				&& Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty())
		&& TestEqual(TEXT("update-existing missing local key captures no rollback actions"),
			Transaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("update-existing missing local key enlists no footprint"),
			Transaction.FootprintEnlistCount, 0)
		&& TestEqual(TEXT("update-existing missing local key preserves every entry"),
			Blackboard->Keys.Num(), MissingPreEntryCount)
		&& TestFalse(TEXT("update-existing true missing key leaves package clean"),
			Blackboard->GetOutermost()->IsDirty());

	Request.KeyName = TEXT("InheritedOnly");
	Blackboard->GetOutermost()->SetDirtyFlag(false);
	FBlackboardLifecycleTransaction InheritedTransaction;
	FBlackboardLifecycleHandleResolver InheritedHandles(InheritedTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> InheritedEditor = ResolveEditorMutableAsset(
		*this, *Blackboard, InheritedTransaction.GetMutationToken(), InheritedHandles);
	if (!InheritedEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> InheritedHandle =
		InheritedHandles.AddProviderLocalAIResult(InheritedEditor.GetValue());
	if (!InheritedHandle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome InheritedOutcome =
		AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
			MakeUpdateExistingPrerequisiteInvocation(InheritedHandle.GetValue()), Request,
			InheritedHandles, &InheritedTransaction);
	Passed &= TestEqual(TEXT("update-existing inherited-only key fails before capture"),
		InheritedOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("update-existing inherited-only key uses KeyInherited"),
		HasDiagnostic(InheritedOutcome, TEXT("AIProvider.Blackboard.KeyInherited")));
	Passed &= TestTrue(TEXT("update-existing inherited-only key returns empty output"),
		InheritedOutcome.Value.ObjectKeys.IsEmpty() && InheritedOutcome.Value.Elements.IsEmpty()
			&& InheritedOutcome.HandleResults.IsEmpty() && InheritedOutcome.ChangedSet.IsEmpty());
	Passed &= TestEqual(TEXT("update-existing inherited-only key captures no rollback actions"),
		InheritedTransaction.ActionEnlistCount, 0);
	Passed &= TestEqual(TEXT("update-existing inherited-only key enlists no footprint"),
		InheritedTransaction.FootprintEnlistCount, 0);
	Passed &= TestFalse(TEXT("update-existing inherited-only key leaves package clean"),
		Blackboard->GetOutermost()->IsDirty());

	const int32 ExistingIndex = Blackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
		{ return Entry.EntryName == TEXT("ExistingKey"); });
	if (!TestTrue(TEXT("update-existing failure fixture retains the existing local key"), ExistingIndex != INDEX_NONE)) return false;
	Blackboard->Keys[ExistingIndex].EntryDescription = TEXT("Before");
	Blackboard->Keys[ExistingIndex].EntryCategory = TEXT("BeforeCategory");
	Blackboard->Keys[ExistingIndex].bInstanceSynced = false;
	Blackboard->GetOutermost()->SetDirtyFlag(false);
	FBlackboardLifecycleTransaction RollbackTransaction;
	FBlackboardLifecycleHandleResolver RollbackHandles(RollbackTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> RollbackEditor = ResolveEditorMutableAsset(
		*this, *Blackboard, RollbackTransaction.GetMutationToken(), RollbackHandles);
	if (!RollbackEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> RollbackHandle =
		RollbackHandles.AddProviderLocalAIResult(RollbackEditor.GetValue());
	if (!TestTrue(TEXT("update-existing rollback fixture obtains an AI mutable Asset handle"), RollbackHandle.IsSet())) return false;
	Request.KeyName = TEXT("ExistingKey");
	for (int32 FailureLeafIndex = 0; FailureLeafIndex < 2; ++FailureLeafIndex)
	{
		UAssetDocumentAITypedPreimageTestLocalKeyType* MutableKey =
			CastChecked<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[ExistingIndex].KeyType);
		MutableKey->Int32Value = 7;
		MutableKey->StringValue = TEXT("Before property");
		Blackboard->Keys[ExistingIndex].EntryDescription = TEXT("Before");
		Blackboard->Keys[ExistingIndex].EntryCategory = TEXT("BeforeCategory");
		Blackboard->Keys[ExistingIndex].bInstanceSynced = false;
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		FBlackboardLifecycleTransaction LeafTransaction;
		FBlackboardLifecycleHandleResolver LeafHandles(LeafTransaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> LeafEditor = ResolveEditorMutableAsset(
			*this, *Blackboard, LeafTransaction.GetMutationToken(), LeafHandles);
		if (!LeafEditor.IsSet()) return false;
		const TOptional<FAssetDocumentOpaqueHandle> LeafHandle =
			LeafHandles.AddProviderLocalAIResult(LeafEditor.GetValue());
		if (!LeafHandle.IsSet()) return false;
		FAssetDocumentPrimitiveOutcome LeafOutcome;
		{
			FScopedBlackboardMetadataWriteTestFailurePoint FailurePoint(
				AssetDocumentAIBlackboardMutation::EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterKeyTypeLeaf,
				FailureLeafIndex);
			LeafOutcome = AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
				MakeUpdateExistingPrerequisiteInvocation(LeafHandle.GetValue()), Request,
				LeafHandles, &LeafTransaction);
		}
		const FString Label = FString::Printf(TEXT("update-existing post-KeyType leaf %d failure"), FailureLeafIndex);
		Passed &= TestEqual(*(Label + TEXT(" returns failure")), LeafOutcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		Passed &= TestTrue(*(Label + TEXT(" uses closed diagnostic")),
			HasDiagnostic(LeafOutcome, TEXT("AIProvider.OperationRejected")));
		Passed &= TestTrue(*(Label + TEXT(" returns empty output")),
			LeafOutcome.Value.ObjectKeys.IsEmpty() && LeafOutcome.Value.Elements.IsEmpty()
				&& LeafOutcome.HandleResults.IsEmpty() && LeafOutcome.ChangedSet.IsEmpty());
		const UAssetDocumentAITypedPreimageTestLocalKeyType* ForwardKey =
			CastChecked<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[ExistingIndex].KeyType);
		Passed &= TestEqual(*(Label + TEXT(" writes the admitted prefix Int32 leaf")), ForwardKey->Int32Value, 41);
		Passed &= TestEqual(*(Label + TEXT(" writes no later String leaf")), ForwardKey->StringValue,
			FailureLeafIndex == 0 ? FString(TEXT("Before property")) : FString(TEXT("Updated property")));
		Passed &= TestTrue(*(Label + TEXT(" writes no metadata leaf")),
			Blackboard->Keys[ExistingIndex].EntryDescription == TEXT("Before")
				&& Blackboard->Keys[ExistingIndex].EntryCategory == TEXT("BeforeCategory")
				&& !Blackboard->Keys[ExistingIndex].bInstanceSynced);
		Passed &= TestEqual(*(Label + TEXT(" captures twice before one footprint")),
			LeafTransaction.Order, TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint")});
		Passed &= TestEqual(*(Label + TEXT(" enlists one ordered two-item footprint")),
			LeafTransaction.EnlistedFootprint.Num(), 2);
		Passed &= TestTrue(*(Label + TEXT(" rolls back through Atomic")),
			LeafTransaction.Rollback(FString::Printf(TEXT("Test.UpdateExistingKeyTypeLeaf%d"), FailureLeafIndex)).bValid);
		const UAssetDocumentAITypedPreimageTestLocalKeyType* RestoredKey =
			CastChecked<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[ExistingIndex].KeyType);
		Passed &= TestTrue(*(Label + TEXT(" rollback restores all state and dirty last")),
			RestoredKey->Int32Value == 7 && RestoredKey->StringValue == TEXT("Before property")
				&& Blackboard->Keys[ExistingIndex].EntryDescription == TEXT("Before")
				&& Blackboard->Keys[ExistingIndex].EntryCategory == TEXT("BeforeCategory")
				&& !Blackboard->Keys[ExistingIndex].bInstanceSynced
				&& !Blackboard->GetOutermost()->IsDirty());
	}

	auto RunMetadataLeafFailure = [&](const TCHAR* Label,
		const AssetDocumentAIBlackboardMutation::EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint FailurePoint,
		const TCHAR* ExpectedCode, const FString& ExpectedDescription,
		const FString& ExpectedCategory, const bool ExpectedInstanceSynced)
	{
		UAssetDocumentAITypedPreimageTestLocalKeyType* MutableKey =
			Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[ExistingIndex].KeyType);
		if (!TestNotNull(*FString::Printf(TEXT("%s fixture retains the KeyType"), Label), MutableKey)) return false;
		MutableKey->Int32Value = 7;
		MutableKey->StringValue = TEXT("Before property");
		Blackboard->Keys[ExistingIndex].EntryDescription = TEXT("Before");
		Blackboard->Keys[ExistingIndex].EntryCategory = TEXT("BeforeCategory");
		Blackboard->Keys[ExistingIndex].bInstanceSynced = false;
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		FBlackboardLifecycleTransaction CandidateTransaction;
		FBlackboardLifecycleHandleResolver CandidateHandles(CandidateTransaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> CandidateEditor = ResolveEditorMutableAsset(
			*this, *Blackboard, CandidateTransaction.GetMutationToken(), CandidateHandles);
		if (!CandidateEditor.IsSet()) return false;
		const TOptional<FAssetDocumentOpaqueHandle> CandidateHandle =
			CandidateHandles.AddProviderLocalAIResult(CandidateEditor.GetValue());
		if (!CandidateHandle.IsSet()) return false;
		FAssetDocumentPrimitiveOutcome CandidateOutcome;
		{
			FScopedBlackboardMetadataWriteTestFailurePoint ScopedFailure(FailurePoint);
			CandidateOutcome = AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
				MakeUpdateExistingPrerequisiteInvocation(CandidateHandle.GetValue()), Request,
				CandidateHandles, &CandidateTransaction);
		}
		bool CandidatePassed = TestEqual(*FString::Printf(TEXT("%s returns failure"), Label),
			CandidateOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed)
			&& TestTrue(*FString::Printf(TEXT("%s uses a closed diagnostic"), Label),
				HasDiagnostic(CandidateOutcome, ExpectedCode))
			&& TestTrue(*FString::Printf(TEXT("%s leaves failure output empty"), Label),
				CandidateOutcome.Value.ObjectKeys.IsEmpty() && CandidateOutcome.Value.Elements.IsEmpty()
				&& CandidateOutcome.HandleResults.IsEmpty() && CandidateOutcome.ChangedSet.IsEmpty())
			&& TestEqual(*FString::Printf(TEXT("%s writes no later metadata leaf"), Label),
				Blackboard->Keys[ExistingIndex].EntryDescription, ExpectedDescription)
			&& TestEqual(*FString::Printf(TEXT("%s preserves the expected category boundary"), Label),
				Blackboard->Keys[ExistingIndex].EntryCategory.ToString(), ExpectedCategory)
			&& TestEqual(*FString::Printf(TEXT("%s preserves the expected instance-sync boundary"), Label),
				static_cast<bool>(Blackboard->Keys[ExistingIndex].bInstanceSynced), ExpectedInstanceSynced)
			&& TestEqual(*FString::Printf(TEXT("%s reaches the preceding KeyType write"), Label),
				CastChecked<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[ExistingIndex].KeyType)->Int32Value, 41)
			&& TestEqual(*FString::Printf(TEXT("%s captures before writes and has one ordered footprint"), Label),
				CandidateTransaction.Order, TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint")});
		CandidatePassed &= TestTrue(*FString::Printf(TEXT("%s rolls back through Atomic"), Label),
			CandidateTransaction.Rollback(FString::Printf(TEXT("Test.%s"), Label)).bValid);
		CandidatePassed &= TestTrue(*FString::Printf(TEXT("%s rollback restores all metadata and dirty state"), Label),
			Blackboard->Keys[ExistingIndex].EntryDescription == TEXT("Before")
			&& Blackboard->Keys[ExistingIndex].EntryCategory == TEXT("BeforeCategory")
			&& !Blackboard->Keys[ExistingIndex].bInstanceSynced
			&& CastChecked<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[ExistingIndex].KeyType)->Int32Value == 7
			&& !Blackboard->GetOutermost()->IsDirty());
		return CandidatePassed;
	};
	Passed &= RunMetadataLeafFailure(TEXT("update-existing post-Description failure"),
		AssetDocumentAIBlackboardMutation::EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterDescription,
		TEXT("AIProvider.OperationRejected"), TEXT("Updated description"), TEXT("BeforeCategory"), false);
	Passed &= RunMetadataLeafFailure(TEXT("update-existing post-Category failure"),
		AssetDocumentAIBlackboardMutation::EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterCategory,
		TEXT("AIProvider.OperationRejected"), TEXT("Updated description"), TEXT("UpdatedCategory"), false);
	Passed &= RunMetadataLeafFailure(TEXT("update-existing post-bInstanceSynced failure"),
		AssetDocumentAIBlackboardMutation::EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterInstanceSynced,
		TEXT("AIProvider.OperationRejected"), TEXT("Updated description"), TEXT("UpdatedCategory"), true);
	Passed &= RunMetadataLeafFailure(TEXT("update-existing post-readback mismatch"),
		AssetDocumentAIBlackboardMutation::EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterPostReadback,
		TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"),
		TEXT("Updated description"), TEXT("UpdatedCategory"), true);

	UBlackboardKeyType_NativeEnum* NativeEnumKey = NewObject<UBlackboardKeyType_NativeEnum>(Blackboard);
	UAssetDocumentAITypedPreimageTestKeyType* UnsupportedGenericKey =
		NewObject<UAssetDocumentAITypedPreimageTestKeyType>(Blackboard);
	UBlackboardKeyType_Object* IncompleteObjectKey = NewObject<UBlackboardKeyType_Object>(Blackboard);
	UBlackboardKeyType_Object* IncompatibleObjectKey = NewObject<UBlackboardKeyType_Object>(Blackboard);
	UBlackboardKeyType_Class* IncompatibleClassKey = NewObject<UBlackboardKeyType_Class>(Blackboard);
	UBlackboardKeyType_Enum* InvalidEnumKey = NewObject<UBlackboardKeyType_Enum>(Blackboard);
	UEnum* TestEnum = StaticEnum<EAssetDocumentAITypedPreimageTestEnum>();
	if (!TestNotNull(TEXT("update-existing NativeEnum rejection fixture exists"), NativeEnumKey)
		|| !TestNotNull(TEXT("update-existing unsupported generic fixture exists"), UnsupportedGenericKey)
		|| !TestNotNull(TEXT("update-existing incomplete Object fixture exists"), IncompleteObjectKey)
		|| !TestNotNull(TEXT("update-existing incompatible Object fixture exists"), IncompatibleObjectKey)
		|| !TestNotNull(TEXT("update-existing incompatible Class fixture exists"), IncompatibleClassKey)
		|| !TestNotNull(TEXT("update-existing invalid Enum fixture exists"), InvalidEnumKey)
		|| !TestNotNull(TEXT("update-existing invalid Enum type fixture exists"), TestEnum)) return false;
	IncompleteObjectKey->BaseClass = nullptr;
	IncompleteObjectKey->DefaultValue = nullptr;
	IncompatibleObjectKey->BaseClass = UObject::StaticClass();
	IncompatibleObjectKey->DefaultValue = nullptr;
	IncompatibleClassKey->BaseClass = UObject::StaticClass();
	IncompatibleClassKey->DefaultValue = nullptr;
	InvalidEnumKey->EnumType = TestEnum;
	InvalidEnumKey->EnumName = TestEnum->GetPathName();
	InvalidEnumKey->bIsEnumNameValid = true;
	InvalidEnumKey->DefaultValue = static_cast<uint8>(EAssetDocumentAITypedPreimageTestEnum::Original);
	for (const TPair<FName, UBlackboardKeyType*>& Entry : {
		TPair<FName, UBlackboardKeyType*>{TEXT("NativeEnumRejected"), NativeEnumKey},
		TPair<FName, UBlackboardKeyType*>{TEXT("UnsupportedGenericRejected"), UnsupportedGenericKey},
		TPair<FName, UBlackboardKeyType*>{TEXT("IncompleteObjectRejected"), IncompleteObjectKey},
		TPair<FName, UBlackboardKeyType*>{TEXT("IncompatibleObjectRejected"), IncompatibleObjectKey},
		TPair<FName, UBlackboardKeyType*>{TEXT("IncompatibleClassRejected"), IncompatibleClassKey},
		TPair<FName, UBlackboardKeyType*>{TEXT("InvalidEnumRejected"), InvalidEnumKey}})
	{
		FBlackboardEntry NewEntry;
		NewEntry.EntryName = Entry.Key;
		NewEntry.KeyType = Entry.Value;
		Blackboard->Keys.Add(NewEntry);
	}
	Blackboard->UpdateParentKeys();
	auto RunCodecPreflightFailure = [&](const TCHAR* Label, const FString& KeyName,
		UBlackboardKeyType& KeyType, const FAssetDocumentRuntimeValueDto& Properties,
		TFunctionRef<bool()> IsUnchanged)
	{
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		FBlackboardLifecycleTransaction CandidateTransaction;
		FBlackboardLifecycleHandleResolver CandidateHandles(CandidateTransaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> CandidateEditor = ResolveEditorMutableAsset(
			*this, *Blackboard, CandidateTransaction.GetMutationToken(), CandidateHandles);
		if (!CandidateEditor.IsSet()) return false;
		const TOptional<FAssetDocumentOpaqueHandle> CandidateHandle =
			CandidateHandles.AddProviderLocalAIResult(CandidateEditor.GetValue());
		if (!CandidateHandle.IsSet()) return false;
		AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest CandidateRequest;
		CandidateRequest.BlackboardPath = Blackboard->GetPathName();
		CandidateRequest.KeyName = KeyName;
		CandidateRequest.ExpectedClassPath = KeyType.GetClass()->GetPathName();
		CandidateRequest.CanonicalProperties = Properties;
		CandidateRequest.Description = TEXT("Must not write");
		CandidateRequest.Category = TEXT("MustNotWrite");
		CandidateRequest.bInstanceSynced = true;
		const FAssetDocumentPrimitiveOutcome CandidateOutcome =
			AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
				MakeUpdateExistingPrerequisiteInvocation(CandidateHandle.GetValue()), CandidateRequest,
				CandidateHandles, &CandidateTransaction);
		return TestEqual(*FString::Printf(TEXT("%s fails closed"), Label), CandidateOutcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed)
			&& TestTrue(*FString::Printf(TEXT("%s uses the authority-unavailable terminal"), Label),
				HasDiagnostic(CandidateOutcome, TEXT("AIProvider.AuthorityUnavailable")))
			&& TestTrue(*FString::Printf(TEXT("%s returns empty failure output"), Label),
				CandidateOutcome.Value.ObjectKeys.IsEmpty() && CandidateOutcome.Value.Elements.IsEmpty()
				&& CandidateOutcome.HandleResults.IsEmpty() && CandidateOutcome.ChangedSet.IsEmpty())
			&& TestEqual(*FString::Printf(TEXT("%s captures no action"), Label),
				CandidateTransaction.ActionEnlistCount, 0)
			&& TestEqual(*FString::Printf(TEXT("%s enlists no footprint"), Label),
				CandidateTransaction.FootprintEnlistCount, 0)
			&& TestTrue(*FString::Printf(TEXT("%s performs no write"), Label), IsUnchanged())
			&& TestFalse(*FString::Printf(TEXT("%s leaves the package clean"), Label),
				Blackboard->GetOutermost()->IsDirty());
	};
	Passed &= RunCodecPreflightFailure(TEXT("update-existing NativeEnum specialized codec"),
		TEXT("NativeEnumRejected"), *NativeEnumKey, ObjectValue({}, {}),
		[&]() { return NativeEnumKey->GetClass() == UBlackboardKeyType_NativeEnum::StaticClass(); });
	Passed &= RunCodecPreflightFailure(TEXT("update-existing unsupported generic codec"),
		TEXT("UnsupportedGenericRejected"), *UnsupportedGenericKey, ObjectValue({}, {}),
		[&]() { return UnsupportedGenericKey->Int32Value == 0 && UnsupportedGenericKey->UnsupportedArrayValue.IsEmpty(); });
	Passed &= RunCodecPreflightFailure(TEXT("update-existing incomplete Object codec"),
		TEXT("IncompleteObjectRejected"), *IncompleteObjectKey,
		ObjectValue({TEXT("BaseClass"), TEXT("DefaultValue")},
			{ReferenceValue(TEXT("ClassRef"), *UObject::StaticClass()), NullValue()}),
		[&]() { return IncompleteObjectKey->BaseClass == nullptr && IncompleteObjectKey->DefaultValue == nullptr; });
	Passed &= RunCodecPreflightFailure(TEXT("update-existing incompatible Object values"),
		TEXT("IncompatibleObjectRejected"), *IncompatibleObjectKey,
		ObjectValue({TEXT("BaseClass"), TEXT("DefaultValue")},
			{ReferenceValue(TEXT("ClassRef"), *AActor::StaticClass()),
				ReferenceValue(TEXT("AssetRef"), *Blackboard)}),
		[&]() { return IncompatibleObjectKey->BaseClass == UObject::StaticClass()
			&& IncompatibleObjectKey->DefaultValue == nullptr; });
	Passed &= RunCodecPreflightFailure(TEXT("update-existing incompatible Class values"),
		TEXT("IncompatibleClassRejected"), *IncompatibleClassKey,
		ObjectValue({TEXT("BaseClass"), TEXT("DefaultValue")},
			{ReferenceValue(TEXT("ClassRef"), *APawn::StaticClass()),
				ReferenceValue(TEXT("ClassRef"), *AActor::StaticClass())}),
		[&]() { return IncompatibleClassKey->BaseClass == UObject::StaticClass()
			&& IncompatibleClassKey->DefaultValue == nullptr; });
	Passed &= RunCodecPreflightFailure(TEXT("update-existing invalid Enum value"),
		TEXT("InvalidEnumRejected"), *InvalidEnumKey,
		ObjectValue({TEXT("DefaultValue"), TEXT("EnumName"), TEXT("EnumType")},
			{StringValue(TEXT("MissingEnumerator")), StringValue(TestEnum->GetPathName()),
				ReferenceValue(TEXT("AssetRef"), *TestEnum)}),
		[&]() { return InvalidEnumKey->EnumType == TestEnum
			&& InvalidEnumKey->DefaultValue == static_cast<uint8>(EAssetDocumentAITypedPreimageTestEnum::Original); });
	Blackboard->Keys.SetNum(2);
	Blackboard->UpdateParentKeys();
	Blackboard->GetOutermost()->SetDirtyFlag(false);

	auto RunAdmissionFailure = [&](const TCHAR* Label, const int32 RejectActionAtCall,
		const int32 RejectFootprintAtCall, const TArray<FString>& ExpectedOrder)
	{
		FBlackboardLifecycleTransaction CandidateTransaction;
		CandidateTransaction.RejectActionAtCall = RejectActionAtCall;
		CandidateTransaction.RejectFootprintAtCall = RejectFootprintAtCall;
		FBlackboardLifecycleHandleResolver CandidateHandles(CandidateTransaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> CandidateEditor = ResolveEditorMutableAsset(
			*this, *Blackboard, CandidateTransaction.GetMutationToken(), CandidateHandles);
		if (!CandidateEditor.IsSet()) return false;
		const TOptional<FAssetDocumentOpaqueHandle> CandidateHandle =
			CandidateHandles.AddProviderLocalAIResult(CandidateEditor.GetValue());
		if (!CandidateHandle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome CandidateOutcome =
			AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
				MakeUpdateExistingPrerequisiteInvocation(CandidateHandle.GetValue()), Request,
				CandidateHandles, &CandidateTransaction);
		return TestEqual(*FString::Printf(TEXT("%s returns failure"), Label), CandidateOutcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed)
			&& TestEqual(*FString::Printf(TEXT("%s stops at its exact admission order"), Label),
				CandidateTransaction.Order, ExpectedOrder)
			&& TestEqual(*FString::Printf(TEXT("%s performs no forward KeyType write"), Label),
				CastChecked<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[ExistingIndex].KeyType)->Int32Value, 7)
			&& TestEqual(*FString::Printf(TEXT("%s performs no metadata write"), Label),
				Blackboard->Keys[ExistingIndex].EntryDescription, FString(TEXT("Before")));
	};
	Passed &= RunAdmissionFailure(TEXT("update-existing LocalKeys capture rejection"), 0, INDEX_NONE,
		{TEXT("action-rejected")});
	Passed &= RunAdmissionFailure(TEXT("update-existing KeyType capture rejection"), 1, INDEX_NONE,
		{TEXT("action"), TEXT("action-rejected")});
	Passed &= RunAdmissionFailure(TEXT("update-existing footprint rejection"), INDEX_NONE, 0,
		{TEXT("action"), TEXT("action"), TEXT("footprint-rejected")});

	AddExistingKey(*Parent, TEXT("MalformedLocal"), 23);
	Parent->UpdateParentKeys();
	Blackboard->UpdateParentKeys();
	FBlackboardEntry MalformedLocalEntry;
	MalformedLocalEntry.EntryName = TEXT("MalformedLocal");
	MalformedLocalEntry.KeyType = nullptr;
	MalformedLocalEntry.EntryDescription = TEXT("Malformed before");
	MalformedLocalEntry.EntryCategory = TEXT("MalformedBeforeCategory");
	MalformedLocalEntry.bInstanceSynced = false;
	Blackboard->Keys.Add(MalformedLocalEntry);
	TArray<FName> MalformedPreNames;
	for (const FBlackboardEntry& Entry : Blackboard->Keys) MalformedPreNames.Add(Entry.EntryName);
	const int32 MalformedLocalIndex = Blackboard->Keys.Num() - 1;
	const int32 MalformedParentEntryCount = Parent->Keys.Num();
	Blackboard->GetOutermost()->SetDirtyFlag(false);
	FBlackboardLifecycleTransaction MalformedTransaction;
	FBlackboardLifecycleHandleResolver MalformedHandles(MalformedTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> MalformedEditor = ResolveEditorMutableAsset(
		*this, *Blackboard, MalformedTransaction.GetMutationToken(), MalformedHandles);
	if (!MalformedEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> MalformedHandle =
		MalformedHandles.AddProviderLocalAIResult(MalformedEditor.GetValue());
	if (!MalformedHandle.IsSet()) return false;
	Request.KeyName = TEXT("MalformedLocal");
	const FAssetDocumentPrimitiveOutcome MalformedOutcome =
		AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
			MakeUpdateExistingPrerequisiteInvocation(MalformedHandle.GetValue()), Request,
			MalformedHandles, &MalformedTransaction);
	TArray<FName> MalformedPostNames;
	for (const FBlackboardEntry& Entry : Blackboard->Keys) MalformedPostNames.Add(Entry.EntryName);
	Passed &= TestEqual(TEXT("update-existing malformed local key fails before capture"),
		MalformedOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("update-existing malformed local key uses AuthorityUnavailable"),
		HasDiagnostic(MalformedOutcome, TEXT("AIProvider.AuthorityUnavailable")));
	Passed &= TestFalse(TEXT("update-existing malformed local key does not use KeyInherited"),
		HasDiagnostic(MalformedOutcome, TEXT("AIProvider.Blackboard.KeyInherited")));
	Passed &= TestFalse(TEXT("update-existing malformed local key does not use KeyMissing"),
		HasDiagnostic(MalformedOutcome, TEXT("AIProvider.Blackboard.KeyMissing")));
	Passed &= TestTrue(TEXT("update-existing malformed local key returns empty output"),
		MalformedOutcome.Value.ObjectKeys.IsEmpty() && MalformedOutcome.Value.Elements.IsEmpty()
			&& MalformedOutcome.HandleResults.IsEmpty() && MalformedOutcome.ChangedSet.IsEmpty());
	Passed &= TestEqual(TEXT("update-existing malformed local key captures no rollback action"),
		MalformedTransaction.ActionEnlistCount, 0);
	Passed &= TestEqual(TEXT("update-existing malformed local key enlists no footprint"),
		MalformedTransaction.FootprintEnlistCount, 0);
	Passed &= TestTrue(TEXT("update-existing malformed local key performs no forward write"),
		Blackboard->Keys.IsValidIndex(MalformedLocalIndex)
			&& Blackboard->Keys[MalformedLocalIndex].EntryName == TEXT("MalformedLocal")
			&& Blackboard->Keys[MalformedLocalIndex].KeyType == nullptr
			&& Blackboard->Keys[MalformedLocalIndex].EntryDescription == TEXT("Malformed before")
			&& Blackboard->Keys[MalformedLocalIndex].EntryCategory == TEXT("MalformedBeforeCategory")
			&& !Blackboard->Keys[MalformedLocalIndex].bInstanceSynced);
	Passed &= TestEqual(TEXT("update-existing malformed local key preserves local order"),
		MalformedPostNames, MalformedPreNames);
	Passed &= TestEqual(TEXT("update-existing malformed local key preserves parent state"),
		Parent->Keys.Num(), MalformedParentEntryCount);
	Passed &= TestFalse(TEXT("update-existing malformed local key leaves package clean"),
		Blackboard->GetOutermost()->IsDirty());
	return Passed;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ParentReadbackTest,
	"UEAssetDoc.AIProvider.BlackboardV2.ParentReadback.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ParentReadbackTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: Parent is a single canonical path/null observation.
	// - Candidate technique: equivalence partitioning for non-null and null Parent.
	// - Risk or gap: a richer v1-style or inherited aggregate may appear valid.
	// - Test: exact one-member output for a live exact Blackboard.
	// - Expected observable outcome: canonical parent_path_or_null and no mutation.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ParentReadback_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Parent = NewObject<UBlackboardData>(
		Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("Parent readback Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("Parent readback Parent fixture exists"), Parent)) return false;
	Blackboard->Parent = Parent;
	Blackboard->UpdateParentKeys();
	Package->SetDirtyFlag(false);
	const FBlackboardObservation Before(*Blackboard);
	FBlackboardLifecycleTransaction Transaction;
	FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
	bool Passed = RequireCanonicalParentReadback(*this, Provider->Invoke(
		MakeParentReadbackInvocation(*Blackboard, Transaction.GetMutationToken()), Handles, nullptr), Parent);
	Passed &= TestTrue(TEXT("Parent readback leaves no transaction footprint"),
		Transaction.EnlistedFootprint.IsEmpty() && Before.Equals(*Blackboard));
	Blackboard->Parent = nullptr;
	Blackboard->UpdateParentKeys();
	Package->SetDirtyFlag(false);
	FBlackboardLifecycleTransaction NullTransaction;
	FBlackboardLifecycleHandleResolver NullHandles(NullTransaction.GetMutationToken());
	Passed &= RequireCanonicalParentReadback(*this, Provider->Invoke(
		MakeParentReadbackInvocation(*Blackboard, NullTransaction.GetMutationToken()), NullHandles, nullptr), nullptr);
	Passed &= TestTrue(TEXT("null Parent readback leaves no transaction footprint"),
		NullTransaction.EnlistedFootprint.IsEmpty() && !Package->IsDirty());
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ParentReadbackFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.ParentReadback.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ParentReadbackFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: all rejected Parent observations are failure-closed.
	// - Candidate technique: error partitioning across envelope, phase, runtime,
	//   transaction and exact-target identity.
	// - Risk or gap: a read-only descriptor could accidentally admit HA/Atomic.
	// - Test: one representative from each rejected input/context class.
	// - Expected observable outcome: empty output and no footprint or dirty state.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ParentReadbackFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* NonBlackboard =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(Package);
	if (!TestNotNull(TEXT("Parent failure Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("Parent failure non-Blackboard fixture exists"), NonBlackboard)) return false;
	Package->SetDirtyFlag(false);
	const FBlackboardObservation Before(*Blackboard);
	bool Passed = true;
	auto RequireFailure = [&](const TCHAR* Label, FAssetDocumentPrimitiveInvocation Invocation,
		IAssetDocumentAtomicTransaction* ActiveTransaction, const TCHAR* Diagnostic = nullptr)
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		Invocation.RequestId = Transaction.GetMutationToken().RequestId;
		Invocation.TransactionId = Transaction.GetMutationToken().TransactionId;
		Invocation.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			Invocation, Handles, ActiveTransaction == nullptr ? nullptr : &Transaction);
		Passed &= RequireReadbackFailure(*this, Label, Outcome, Before, *Blackboard,
			Transaction, Diagnostic);
	};
	{
		FBlackboardLifecycleTransaction Token;
		RequireFailure(TEXT("Parent readback rejects Apply"),
			MakeParentReadbackInvocation(*Blackboard, Token.GetMutationToken(),
				EAssetDocumentRuntimeRequestPhase::Apply), nullptr);
		FAssetDocumentPrimitiveInvocation Malformed =
			MakeParentReadbackInvocation(*Blackboard, Token.GetMutationToken());
		Malformed.ValueArguments = ObjectValue({}, {});
		RequireFailure(TEXT("Parent readback rejects malformed request"), MoveTemp(Malformed), nullptr,
			TEXT("AIProvider.InvalidValueShape"));
		FAssetDocumentPrimitiveInvocation WithHandle =
			MakeParentReadbackInvocation(*Blackboard, Token.GetMutationToken());
		FAssetDocumentOpaqueHandle Unexpected;
		Unexpected.Kind = EAssetDocumentHandleKind::Asset;
		Unexpected.Access = EAssetDocumentHandleAccess::Mutable;
		WithHandle.HandleArguments = {{TEXT("asset"), Unexpected}};
		RequireFailure(TEXT("Parent readback rejects supplied handle"), MoveTemp(WithHandle), nullptr,
			TEXT("AIProvider.InvalidHandle"));
		FAssetDocumentPrimitiveInvocation Missing =
			MakeParentReadbackInvocation(*Blackboard, Token.GetMutationToken());
		Missing.ValueArguments.Elements[0].StringValue =
			TEXT("/Temp/AssetDocumentAI/Missing.Missing");
		RequireFailure(TEXT("Parent readback rejects unavailable target"), MoveTemp(Missing), nullptr,
			TEXT("AIProvider.Blackboard.ParentUnavailable"));
		RequireFailure(TEXT("Parent readback rejects non-Blackboard target"),
			MakeParentReadbackInvocation(*NonBlackboard, Token.GetMutationToken()), nullptr,
			TEXT("AIProvider.Blackboard.ParentUnavailable"));
	}
	{
		FBlackboardLifecycleTransaction Active;
		FBlackboardLifecycleHandleResolver Handles(Active.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeParentReadbackInvocation(*Blackboard, Active.GetMutationToken()), Handles, &Active);
		Passed &= RequireReadbackFailure(*this, TEXT("Parent readback rejects active transaction"),
			Outcome, Before, *Blackboard, Active, TEXT("AIProvider.InvalidTransaction"));
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const bool bPreviousEditor = GIsEditor;
		GIsEditor = false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeParentReadbackInvocation(*Blackboard, Transaction.GetMutationToken()), Handles, nullptr);
		GIsEditor = bPreviousEditor;
		Passed &= RequireReadbackFailure(*this, TEXT("Parent readback rejects non-Editor context"),
			Outcome, Before, *Blackboard, Transaction, TEXT("AIProvider.InvalidContext"));
	}
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2AddSuccessTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Add.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2AddSuccessTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2AddSuccess_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("entry add success Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("entry add success child fixture exists"), Child)) return false;
	TStrongObjectPtr<UBlackboardData> BlackboardGuard(Blackboard);
	TStrongObjectPtr<UBlackboardData> ChildGuard(Child);
	AddExistingKey(*Blackboard, TEXT("First"), 1);
	AddExistingKey(*Blackboard, TEXT("Last"), 2);
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Blackboard->GetOutermost()->SetDirtyFlag(false);

	auto RunAdd = [&](const TCHAR* Label, const FString& Name, const int32 Index,
		const TArray<FString>& ExpectedOrder)
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentRuntimeValueDto ExpectedEntry = MakeEntryAddV2Entry(Name);
		CollectGarbage(RF_NoFlags);
		const int32 DirectKeyTypesBefore = CountDirectBlackboardKeyTypes(*Blackboard);
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryAddV2Invocation(Handle.GetValue(), *Blackboard, ExpectedEntry, Index),
			Handles, &Transaction);
		TArray<FAssetDocumentRuntimeValueDto> OrderedNames;
		for (const FString& OrderedName : ExpectedOrder) OrderedNames.Add(StringValue(OrderedName));
		const FAssetDocumentRuntimeValueDto ExpectedOutput = ObjectValue(
			{TEXT("entry"), TEXT("ordered_local_key_names")},
			{ExpectedEntry, ArrayValue(MoveTemp(OrderedNames))});
		const int32 ActualIndex = Blackboard->Keys.IndexOfByPredicate([&Name](const FBlackboardEntry& Entry)
			{ return Entry.EntryName.ToString() == Name; });
		int32 ActualAuthoredIndex = 0;
		for (int32 PhysicalIndex = 0; PhysicalIndex < ActualIndex; ++PhysicalIndex)
		{
			ActualAuthoredIndex += !IsEngineDerivedSelfKey(Blackboard->Keys[PhysicalIndex]);
		}
		const UAssetDocumentAITypedPreimageTestLocalKeyType* KeyType =
			ActualIndex == INDEX_NONE ? nullptr
				: Cast<UAssetDocumentAITypedPreimageTestLocalKeyType>(Blackboard->Keys[ActualIndex].KeyType);
		bool Passed = RequireSuccess(*this, Label, Outcome)
			&& TestTrue(*(FString(Label) + TEXT(" returns exact canonical entry/order")),
				RuntimeValueEquals(Outcome.Value, ExpectedOutput))
			&& TestEqual(*(FString(Label) + TEXT(" inserts at exact authored-local index")),
				ActualAuthoredIndex, Index)
			&& TestNotNull(*(FString(Label) + TEXT(" materializes exact KeyType class")), KeyType)
			&& (KeyType == nullptr || TestTrue(*(FString(Label) + TEXT(" configures complete KeyType state")),
				KeyType->Int32Value == 41 && KeyType->StringValue == TEXT("Added property")))
			&& TestEqual(*(FString(Label) + TEXT(" captures before one complete footprint")),
				Transaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")})
			&& TestEqual(*(FString(Label) + TEXT(" enlists exactly one footprint")),
				Transaction.EnlistedFootprint.Num(), 1)
			&& TestEqual(*(FString(Label) + TEXT(" reports entry and order changes")),
				Outcome.ChangedSet.Num(), 2)
			&& TestEqual(*(FString(Label) + TEXT(" retains exactly one published KeyType")),
				CountDirectBlackboardKeyTypes(*Blackboard), DirectKeyTypesBefore + 1)
			&& TestTrue(*(FString(Label) + TEXT(" completes synchronized-key derived state")),
				Blackboard->HasSynchronizedKeys())
			&& TestTrue(*(FString(Label) + TEXT(" propagates exact child Key-ID")),
				Child->GetKeyID(FName(Name)) != FBlackboard::InvalidKey
					&& Child->GetKey(Child->GetKeyID(FName(Name))) != nullptr)
			&& TestEqual(*(FString(Label) + TEXT(" propagates exact child KeyType")),
				Child->GetKeyType(Child->GetKeyID(FName(Name))).Get(),
				UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass())
			&& TestTrue(*(FString(Label) + TEXT(" propagates synchronized-key state to child")),
				Child->HasSynchronizedKeys()
					&& Child->IsKeyInstanceSynced(Child->GetKeyID(FName(Name))));
		if (Transaction.EnlistedFootprint.Num() == 1)
		{
			Passed &= TestEqual(*(FString(Label) + TEXT(" uses exact LocalKeysAggregate component")),
				Transaction.EnlistedFootprint[0].RollbackComponentId,
				FString(TEXT("assetdocument.ai.blackboard.entry.add.v2")));
		}
		if (Outcome.ChangedSet.Num() == 2)
		{
			Passed &= TestEqual(*(FString(Label) + TEXT(" reports exact named-entry path first")),
				Outcome.ChangedSet[0].Path,
				FString::Printf(TEXT("asset:%s/Keys[name=%s]"), *Blackboard->GetPathName(), *Name));
			Passed &= TestEqual(*(FString(Label) + TEXT(" reports exact order path second")),
				Outcome.ChangedSet[1].Path,
				FString::Printf(TEXT("asset:%s/Keys.@Order"), *Blackboard->GetPathName()));
			for (const FAssetDocumentChangedRecord& Change : Outcome.ChangedSet)
			{
				Passed &= TestEqual(*(FString(Label) + TEXT(" reports exact ChangedSet authority")),
					Change.AuthorityId, FString(TEXT("ai.blackboard.entry.add.v2")));
				Passed &= TestEqual(*(FString(Label) + TEXT(" reports exact ChangedSet object")),
					Change.StableObjectId, Blackboard->GetPathName());
				Passed &= TestEqual(*(FString(Label) + TEXT(" reports update ChangeKind")),
					Change.ChangeKind, FString(TEXT("Update")));
			}
		}
		return Passed;
	};

	bool Passed = RunAdd(TEXT("entry add index zero"), TEXT("Head"), 0,
		{TEXT("Head"), TEXT("First"), TEXT("Last")});
	Passed &= RunAdd(TEXT("entry add middle index"), TEXT("Middle"), 2,
		{TEXT("Head"), TEXT("First"), TEXT("Middle"), TEXT("Last")});
	Passed &= RunAdd(TEXT("entry add end index"), TEXT("Tail"), 4,
		{TEXT("Head"), TEXT("First"), TEXT("Middle"), TEXT("Last"), TEXT("Tail")});

	// Test design:
	// - Behavior under test: public staged add publishes one complete admitted
	//   Struct KeyType entry and rolls it back without leaving a staged child.
	// - Candidate technique: state transition with exact public DTO and rollback
	//   observations for the parent Blackboard and its derived child.
	// - Risk or gap: generic leaf writes can reject Struct DefaultValue before
	//   staging, or publication can appear to succeed without exact canonical
	//   output, propagated state, or Atomic cleanup.
	// - Test: add StructCanonical at the authored-local end, then roll
	//   back that same successful transaction.
	// - Expected observable outcome: exact entry/order DTO, one LocalKeys
	//   footprint, an owned native Struct value, and no post-rollback residue.
	const FString StructAddedName = TEXT("StructAdded");
	const FAssetDocumentRuntimeValueDto StructAddedDefault = ObjectValue(
		{TEXT("Properties"), TEXT("Struct")},
		{ObjectValue({TEXT("Count"), TEXT("Label")},
			{IntegerValue(57), StringValue(TEXT("added-struct"))}),
			StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())});
	const FAssetDocumentRuntimeValueDto StructAddedEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("StructAddedCategory")), StringValue(TEXT("Struct added description")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {StructAddedDefault}),
			StringValue(StructAddedName), BooleanValue(true)});
	const FAssetDocumentRuntimeValueDto StructAddedOutput = ObjectValue(
		{TEXT("entry"), TEXT("ordered_local_key_names")},
		{StructAddedEntry, ArrayValue({StringValue(TEXT("Head")), StringValue(TEXT("First")),
			StringValue(TEXT("Middle")), StringValue(TEXT("Last")), StringValue(TEXT("Tail")),
			StringValue(StructAddedName)})});
	Blackboard->GetOutermost()->SetDirtyFlag(false);
	const FBlackboardObservation StructAddedBefore(*Blackboard);
	const FBlackboardObservation StructAddedChildBefore(*Child);
	CollectGarbage(RF_NoFlags);
	const int32 StructAddedDirectKeyTypesBefore = CountDirectBlackboardKeyTypes(*Blackboard);
	FBlackboardLifecycleTransaction StructAddedTransaction;
	FBlackboardLifecycleHandleResolver StructAddedHandles(StructAddedTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> StructAddedHandle = ResolveAIMutableAsset(
		*this, *Blackboard, StructAddedTransaction, StructAddedHandles);
	if (!StructAddedHandle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome StructAddedOutcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryAddV2Invocation(StructAddedHandle.GetValue(), *Blackboard, StructAddedEntry,
			CountAuthoredLocalKeys(*Blackboard)),
		StructAddedHandles, &StructAddedTransaction);
	bool StructAddedPassed = RequireSuccess(*this, TEXT("entry add StructCanonical"), StructAddedOutcome);
	if (StructAddedOutcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		const FString FirstDiagnostic = StructAddedOutcome.Diagnostics.IsEmpty()
			? TEXT("<none>") : StructAddedOutcome.Diagnostics[0].Code;
		AddInfo(FString::Printf(TEXT("entry add StructCanonical diagnostic: %s"),
			*FirstDiagnostic));
		if (StructAddedTransaction.ActionEnlistCount != 0)
		{
			const FAssetDocumentContractValidationResult CleanupResult =
				StructAddedTransaction.Rollback(TEXT("Test.EntryAddStructCanonicalRedCleanup"));
			for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : CleanupResult.Diagnostics)
			{
				AddInfo(FString::Printf(TEXT("entry add StructCanonical cleanup diagnostic %s: %s"),
					*Diagnostic.Code, *Diagnostic.Message));
			}
			StructAddedPassed &= TestTrue(TEXT("entry add StructCanonical failed-product cleanup succeeds"),
				CleanupResult.bValid);
			StructAddedPassed &= RequireObservation(*this,
				TEXT("entry add StructCanonical failed-product cleanup restores parent"),
				StructAddedBefore, *Blackboard);
			StructAddedPassed &= RequireObservation(*this,
				TEXT("entry add StructCanonical failed-product cleanup restores child"),
				StructAddedChildBefore, *Child);
		}
	}
	else
	{
		const int32 StructAddedIndex = Blackboard->Keys.IndexOfByPredicate(
			[&StructAddedName](const FBlackboardEntry& Entry)
			{ return Entry.EntryName.ToString() == StructAddedName; });
		int32 StructAddedNameCount = 0;
		for (const FBlackboardEntry& Entry : Blackboard->Keys)
		{
			StructAddedNameCount += Entry.EntryName.ToString() == StructAddedName;
		}
		const UBlackboardKeyType_Struct* StructAddedKeyType = StructAddedIndex == INDEX_NONE
			? nullptr : Cast<UBlackboardKeyType_Struct>(Blackboard->Keys[StructAddedIndex].KeyType);
		const FAssetDocumentAIBlackboardCanonicalStructNested* StructAddedValue =
			StructAddedKeyType != nullptr
				? StructAddedKeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>()
				: nullptr;
		const FBlackboard::FKey StructAddedChildKeyId = Child->GetKeyID(FName(StructAddedName));
		StructAddedPassed &= TestTrue(TEXT("entry add StructCanonical returns exact canonical entry/order"),
			RuntimeValueEquals(StructAddedOutcome.Value, StructAddedOutput))
			&& TestTrue(TEXT("entry add StructCanonical returns no handles"),
				StructAddedOutcome.HandleResults.IsEmpty())
			&& TestEqual(TEXT("entry add StructCanonical reports exactly two changes"),
				StructAddedOutcome.ChangedSet.Num(), 2)
			&& TestEqual(TEXT("entry add StructCanonical captures one LocalKeys action and footprint"),
				StructAddedTransaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")})
			&& TestEqual(TEXT("entry add StructCanonical enlists one LocalKeys footprint"),
				StructAddedTransaction.EnlistedFootprint.Num(), 1)
			&& TestEqual(TEXT("entry add StructCanonical publishes exactly one named entry"),
				StructAddedNameCount, 1)
			&& TestEqual(TEXT("entry add StructCanonical inserts at the authored-local end"),
				StructAddedIndex, Blackboard->Keys.Num() - 1)
			&& TestEqual(TEXT("entry add StructCanonical retains one published direct KeyType"),
				CountDirectBlackboardKeyTypes(*Blackboard), StructAddedDirectKeyTypesBefore + 1)
			&& TestNotNull(TEXT("entry add StructCanonical publishes the exact Struct KeyType"),
				StructAddedKeyType)
			&& TestEqual(TEXT("entry add StructCanonical owns the staged Struct KeyType directly"),
				StructAddedKeyType == nullptr ? nullptr : StructAddedKeyType->GetOuter(),
				static_cast<UObject*>(Blackboard))
			&& TestTrue(TEXT("entry add StructCanonical writes exact native nested Struct value"),
				StructAddedValue != nullptr && StructAddedValue->Count == 57
					&& StructAddedValue->Label == TEXT("added-struct"))
			&& TestTrue(TEXT("entry add StructCanonical propagates child Key-ID/class/sync state"),
				StructAddedChildKeyId != FBlackboard::InvalidKey
					&& Child->GetKey(StructAddedChildKeyId) != nullptr
					&& Child->GetKeyType(StructAddedChildKeyId).Get()
						== UBlackboardKeyType_Struct::StaticClass()
					&& Child->IsKeyInstanceSynced(StructAddedChildKeyId));
		if (StructAddedOutcome.ChangedSet.Num() == 2)
		{
			StructAddedPassed &= TestEqual(TEXT("entry add StructCanonical reports named-entry path first"),
				StructAddedOutcome.ChangedSet[0].Path,
				FString::Printf(TEXT("asset:%s/Keys[name=%s]"), *Blackboard->GetPathName(), *StructAddedName))
				&& TestEqual(TEXT("entry add StructCanonical reports order path second"),
					StructAddedOutcome.ChangedSet[1].Path,
					FString::Printf(TEXT("asset:%s/Keys.@Order"), *Blackboard->GetPathName()))
				&& TestEqual(TEXT("entry add StructCanonical reports exact ChangedSet authority first"),
					StructAddedOutcome.ChangedSet[0].AuthorityId, FString(EntryAddV2Id))
				&& TestEqual(TEXT("entry add StructCanonical reports exact ChangedSet authority second"),
					StructAddedOutcome.ChangedSet[1].AuthorityId, FString(EntryAddV2Id));
			for (const FAssetDocumentChangedRecord& Change : StructAddedOutcome.ChangedSet)
			{
				StructAddedPassed &= TestEqual(TEXT("entry add StructCanonical reports exact ChangedSet object"),
					Change.StableObjectId, Blackboard->GetPathName());
				StructAddedPassed &= TestEqual(TEXT("entry add StructCanonical reports Update ChangedSet kind"),
					Change.ChangeKind, FString(TEXT("Update")));
			}
		}
		if (StructAddedTransaction.EnlistedFootprint.Num() == 1)
		{
			StructAddedPassed &= TestEqual(TEXT("entry add StructCanonical uses exact LocalKeys component"),
				StructAddedTransaction.EnlistedFootprint[0].RollbackComponentId,
				FString(TEXT("assetdocument.ai.blackboard.entry.add.v2")));
		}
		const FAssetDocumentContractValidationResult StructAddedRollbackResult =
			StructAddedTransaction.Rollback(TEXT("Test.EntryAddStructCanonicalRollback"));
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : StructAddedRollbackResult.Diagnostics)
		{
			AddError(FString::Printf(TEXT("entry add StructCanonical rollback diagnostic %s: %s"),
				*Diagnostic.Code, *Diagnostic.Message));
		}
		StructAddedPassed &= TestTrue(TEXT("entry add StructCanonical rollback succeeds"),
			StructAddedRollbackResult.bValid);
		StructAddedPassed &= RequireObservation(*this,
			TEXT("entry add StructCanonical rollback restores exact parent preimage"),
			StructAddedBefore, *Blackboard);
		StructAddedPassed &= RequireObservation(*this,
			TEXT("entry add StructCanonical rollback restores exact child derived state"),
			StructAddedChildBefore, *Child);
		CollectGarbage(RF_NoFlags);
		StructAddedPassed &= TestEqual(TEXT("entry add StructCanonical rollback leaves no staged direct child"),
			CountDirectBlackboardKeyTypes(*Blackboard), StructAddedDirectKeyTypesBefore);
	}
	Passed &= StructAddedPassed;

	Blackboard->GetOutermost()->SetDirtyFlag(false);
	const FBlackboardObservation BeforeLateRollback(*Blackboard);
	const FBlackboardObservation ChildBeforeLateRollback(*Child);
	FBlackboardLifecycleTransaction LateTransaction;
	FBlackboardLifecycleHandleResolver LateHandles(LateTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> LateHandle = ResolveAIMutableAsset(
		*this, *Blackboard, LateTransaction, LateHandles);
	if (!LateHandle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome Late = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryAddV2Invocation(LateHandle.GetValue(), *Blackboard,
			MakeEntryAddV2Entry(TEXT("LateRollback")),
			CountAuthoredLocalKeys(*Blackboard)),
		LateHandles, &LateTransaction);
	Passed &= RequireSuccess(*this, TEXT("entry add before late rollback"), Late);
	const FAssetDocumentContractValidationResult LateRollbackResult =
		LateTransaction.Rollback(TEXT("Test.EntryAddLateRollback"));
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : LateRollbackResult.Diagnostics)
	{
		AddError(FString::Printf(TEXT("entry add late rollback diagnostic %s: %s"),
			*Diagnostic.Code, *Diagnostic.Message));
	}
	Passed &= TestTrue(TEXT("entry add late rollback succeeds"), LateRollbackResult.bValid);
	Passed &= RequireObservation(*this, TEXT("entry add late rollback restores exact preimage"),
		BeforeLateRollback, *Blackboard);
	Passed &= RequireObservation(*this, TEXT("entry add late rollback restores child derived state"),
		ChildBeforeLateRollback, *Child);
	CollectGarbage(RF_NoFlags);
	Passed &= TestEqual(TEXT("entry add late rollback leaves no staged direct child"),
		CountDirectBlackboardKeyTypes(*Blackboard), BeforeLateRollback.Instances.Num());
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2AddSaveReloadTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Add.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2AddSaveReloadTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentAIBlackboardAdd/BlackboardAddReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PackageFileName, false, true);
	};

	UPackage* Package = CreatePackage(*PackageName);
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("entry add save-reload Blackboard exists"), Blackboard)) return false;
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Package->SetDirtyFlag(false);

	const FString AddedName = TEXT("PersistedInt");
	const int32 DefaultValue = 73;
	const FAssetDocumentRuntimeValueDto ExpectedEntry =
		MakeEntryAddV2IntEntry(AddedName, DefaultValue);
	bool Passed = true;
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryAddV2Invocation(
				Handle.GetValue(), *Blackboard, ExpectedEntry, 0),
			Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("entry add before save-reload"), Outcome);
		Passed &= TestTrue(TEXT("entry add save-reload output contains canonical entry"),
			Outcome.Value.Elements.Num() == 2
				&& RuntimeValueEquals(Outcome.Value.Elements[0], ExpectedEntry));

		FBlackboardKeySelector Selector;
		Selector.SelectedKeyName = FName(AddedName);
		Selector.ResolveSelectedKey(*Blackboard);
		Passed &= TestEqual(TEXT("entry add resolves BehaviorTree selector Key-ID"),
			Selector.GetSelectedKeyID(), Blackboard->GetKeyID(FName(AddedName)));
		Passed &= TestEqual(TEXT("entry add resolves BehaviorTree selector type"),
			Selector.SelectedKeyType.Get(), UBlackboardKeyType_Int::StaticClass());

		UWorld* TestWorld = GEditor != nullptr
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		Passed &= TestNotNull(TEXT("entry add value-or-key has an Editor world"), TestWorld);
		UBlackboardComponent* BlackboardComponent =
			TestWorld != nullptr ? NewObject<UBlackboardComponent>(TestWorld) : nullptr;
		Passed &= TestTrue(TEXT("entry add initializes value-or-key Blackboard"),
			BlackboardComponent != nullptr
				&& BlackboardComponent->InitializeBlackboard(*Blackboard));
		if (BlackboardComponent != nullptr)
		{
			FValueOrBBKey_Int32 ValueOrKey(-1);
			ValueOrKey.SetKey(FName(AddedName));
			Passed &= TestEqual(TEXT("entry add value-or-key observes configured default"),
				ValueOrKey.GetValue(*BlackboardComponent), DefaultValue);
			BlackboardComponent->SetValueAsInt(FName(AddedName), 81);
			Passed &= TestEqual(TEXT("entry add value-or-key resolves runtime value"),
				ValueOrKey.GetValue(*BlackboardComponent), 81);
		}

		Passed &= TestTrue(TEXT("entry add transaction becomes commit-ready"),
			Transaction.MarkReadyToCommit().bValid);
		Passed &= TestTrue(TEXT("entry add transaction commits before persistence"),
			Transaction.Commit(MakeStructuralCompletionReceipt(Transaction)).bValid);
	}

	Package->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	Passed &= TestTrue(TEXT("entry add result saves through the external lifecycle"),
		UPackage::SavePackage(Package, Blackboard, *PackageFileName, SaveArgs));
	if (!Passed) return false;

	Passed &= TestTrue(TEXT("entry add package unloads before canonical reload"),
		UPackageTools::UnloadPackages({Package}));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	UBlackboardData* Reloaded = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("entry add Blackboard fresh-reloads"), Reloaded)) return false;
	const FBlackboardEntry* ReloadedEntry = Reloaded->Keys.FindByPredicate(
		[&AddedName](const FBlackboardEntry& Entry)
		{
			return Entry.EntryName.ToString() == AddedName;
		});
	if (!TestNotNull(TEXT("fresh reload retains added entry"), ReloadedEntry)) return false;
	const UBlackboardKeyType_Int* ReloadedKeyType =
		Cast<UBlackboardKeyType_Int>(ReloadedEntry->KeyType);
	Passed &= TestNotNull(TEXT("fresh reload retains exact KeyType class"), ReloadedKeyType);
	Passed &= TestEqual(TEXT("fresh reload retains configured KeyType state"),
		ReloadedKeyType != nullptr ? ReloadedKeyType->DefaultValue : INDEX_NONE,
		DefaultValue);
	Passed &= TestEqual(TEXT("fresh reload retains Description"),
		ReloadedEntry->EntryDescription, FString(TEXT("Survives fresh reload")));
	Passed &= TestEqual(TEXT("fresh reload retains Category"),
		ReloadedEntry->EntryCategory, FName(TEXT("Persistence")));
	Passed &= TestTrue(TEXT("fresh reload retains bInstanceSynced"),
		ReloadedEntry->bInstanceSynced != 0 && Reloaded->HasSynchronizedKeys());

	FBlackboardKeySelector ReloadedSelector;
	ReloadedSelector.SelectedKeyName = FName(AddedName);
	ReloadedSelector.ResolveSelectedKey(*Reloaded);
	Passed &= TestEqual(TEXT("fresh reload resolves selector Key-ID"),
		ReloadedSelector.GetSelectedKeyID(), Reloaded->GetKeyID(FName(AddedName)));
	Passed &= TestEqual(TEXT("fresh reload resolves selector type"),
		ReloadedSelector.SelectedKeyType.Get(), UBlackboardKeyType_Int::StaticClass());

	FBlackboardLifecycleTransaction ReadbackTransaction;
	FBlackboardLifecycleHandleResolver ReadbackHandles(
		ReadbackTransaction.GetMutationToken());
	const FAssetDocumentPrimitiveOutcome Readback =
		CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryReadbackInvocation(
				*Reloaded, ReadbackTransaction.GetMutationToken(), AddedName),
			ReadbackHandles, nullptr);
	Passed &= RequireSuccess(*this, TEXT("fresh reload canonical entry readback"), Readback);
	Passed &= TestTrue(TEXT("fresh reload canonical entry equals add output"),
		Readback.Value.ObjectKeys == TArray<FString>{TEXT("entry")}
			&& Readback.Value.Elements.Num() == 1
			&& RuntimeValueEquals(Readback.Value.Elements[0], ExpectedEntry));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2AddFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Add.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2AddFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2AddFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Parent = NewObject<UBlackboardData>(
		Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("entry add failure Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("entry add failure parent fixture exists"), Parent)
		|| !TestNotNull(TEXT("entry add failure child fixture exists"), Child)) return false;
	TStrongObjectPtr<UBlackboardData> BlackboardGuard(Blackboard);
	TStrongObjectPtr<UBlackboardData> ParentGuard(Parent);
	TStrongObjectPtr<UBlackboardData> ChildGuard(Child);
	AddExistingKey(*Blackboard, TEXT("Existing"), 7);
	AddExistingKey(*Blackboard, TEXT("Shadowed"), 8);
	AddExistingKey(*Parent, TEXT("Inherited"), 9);
	AddExistingKey(*Parent, TEXT("Shadowed"), 10);
	Parent->UpdateParentKeys();
	Parent->UpdateKeyIDs();
	Parent->UpdateIfHasSynchronizedKeys();
	Blackboard->Parent = Parent;
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Parent->PropagateKeyChangesToDerivedBlackboardAssets();
	Blackboard->GetOutermost()->SetDirtyFlag(false);

	auto RunPreflightFailure = [&](const TCHAR* Label,
		FAssetDocumentRuntimeValueDto Entry, const int64 Index, const TCHAR* ExpectedCode,
		TFunction<void(FAssetDocumentPrimitiveInvocation&)> Mutate = {})
	{
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveInvocation Invocation = MakeEntryAddV2Invocation(
			Handle.GetValue(), *Blackboard, MoveTemp(Entry), Index);
		if (Mutate) Mutate(Invocation);
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			Invocation, Handles, &Transaction);
		const bool bTerminalPassed = RequireExactFailureTerminal(*this, Label, Outcome, Before,
			*Blackboard, Transaction, ExpectedCode)
			&& TestEqual(*(FString(Label) + TEXT(" captures no action")),
				Transaction.ActionEnlistCount, 0)
			&& TestEqual(*(FString(Label) + TEXT(" enlists no footprint")),
				Transaction.FootprintEnlistCount, 0);
		if (!bTerminalPassed && Transaction.ActionEnlistCount != 0)
		{
			Transaction.Rollback(TEXT("Test.EntryAddPreflightRedCleanup"));
		}
		return bTerminalPassed;
	};

	bool Passed = RunPreflightFailure(TEXT("entry add rejects negative index"),
		MakeEntryAddV2Entry(TEXT("Negative")), -1, TEXT("AIProvider.Blackboard.EntrySchemaInvalid"));
	Passed &= RunPreflightFailure(TEXT("entry add rejects index beyond end"),
		MakeEntryAddV2Entry(TEXT("Beyond")), Blackboard->Keys.Num() + 1,
		TEXT("AIProvider.Blackboard.EntrySchemaInvalid"));
	Passed &= RunPreflightFailure(TEXT("entry add rejects duplicate local name"),
		MakeEntryAddV2Entry(TEXT("Existing")), 0, TEXT("AIProvider.Blackboard.KeyAlreadyExists"));
	Passed &= RunPreflightFailure(TEXT("entry add rejects inherited-only name"),
		MakeEntryAddV2Entry(TEXT("Inherited")), 0, TEXT("AIProvider.Blackboard.KeyInherited"));
	Passed &= RunPreflightFailure(TEXT("entry add rejects local shadow name"),
		MakeEntryAddV2Entry(TEXT("Shadowed")), 0, TEXT("AIProvider.Blackboard.KeyAlreadyExists"));
	Passed &= RunPreflightFailure(TEXT("entry add rejects NAME_None spelling"),
		MakeEntryAddV2Entry(TEXT("None")), 0, TEXT("AIProvider.Blackboard.EntrySchemaInvalid"));
	Passed &= RunPreflightFailure(TEXT("entry add rejects unrelated parent-shadowed local snapshot"),
		MakeEntryAddV2Entry(TEXT("Fresh")), 0, TEXT("AIProvider.AuthorityUnavailable"));

	// The inherited/local same-name pair above is deliberately malformed input
	// only for the shadow preflight lanes.  Restore a canonical snapshot before
	// independently testing class/schema and materialized rollback terminals.
	Blackboard->Keys.RemoveAll([](const FBlackboardEntry& Entry)
	{
		return Entry.EntryName == TEXT("Shadowed");
	});
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Parent->PropagateKeyChangesToDerivedBlackboardAssets();
	Blackboard->GetOutermost()->SetDirtyFlag(false);

	Passed &= RunPreflightFailure(TEXT("entry add rejects non-KeyType class"),
		MakeEntryAddV2Entry(TEXT("BadClass")), 0, TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments.Elements[1].Elements[2].Elements[1] =
				StringValue(UObject::StaticClass()->GetPathName());
		});
	Passed &= RunPreflightFailure(TEXT("entry add rejects non-canonical insertion kind"),
		MakeEntryAddV2Entry(TEXT("BadInsertion")), 0, TEXT("AIProvider.InvalidValueShape"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments.Elements[2].Elements[1] = StringValue(TEXT("Append"));
		});
	const FAssetDocumentRuntimeValueDto StructDependencyEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("")), StringValue(TEXT("")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({TEXT("DefaultValue")},
				{ObjectValue({TEXT("Properties"), TEXT("Struct")},
					{ObjectValue({TEXT("Count"), TEXT("Label")},
						{IntegerValue(41), StringValue(TEXT("missing-dependency"))}),
						StringValue(UObject::StaticClass()->GetPathName())})}),
			StringValue(TEXT("StructDependency")), BooleanValue(false)});
	Passed &= RunPreflightFailure(TEXT("entry add Struct dependency refusal is materialization unavailable"),
		StructDependencyEntry, CountAuthoredLocalKeys(*Blackboard),
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));

	auto RunMaterializedFailure = [&](const TCHAR* Label,
		const int32 FailureOrdinal, const TCHAR* ExpectedCode,
		FAssetDocumentRuntimeValueDto Entry = MakeEntryAddV2Entry(TEXT("Injected")))
	{
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		CollectGarbage(RF_NoFlags);
		const int32 DirectKeyTypesBefore = CountDirectBlackboardKeyTypes(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		SetAssetDocumentAIBlackboardTestFailurePoint(
			static_cast<EAssetDocumentAIBlackboardTestFailurePoint>(FailureOrdinal));
		const int32 AuthoredLocalCount = CountAuthoredLocalKeys(*Blackboard);
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryAddV2Invocation(Handle.GetValue(), *Blackboard, MoveTemp(Entry), AuthoredLocalCount),
			Handles, &Transaction);
		SetAssetDocumentAIBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::None);
		bool LocalPassed = TestEqual(*(FString(Label) + TEXT(" fails")), Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed)
			&& TestTrue(*(FString(Label) + TEXT(" uses exact closed terminal")), HasDiagnostic(Outcome, ExpectedCode))
			&& TestTrue(*(FString(Label) + TEXT(" clears output/handles/ChangedSet")),
				Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty()
					&& Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty())
			&& TestEqual(*(FString(Label) + TEXT(" captures then enlists exactly once")),
				Transaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")});
		const FAssetDocumentContractValidationResult RollbackResult =
			Transaction.Rollback(FString::Printf(TEXT("Test.EntryAddFailure.%d"), FailureOrdinal));
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : RollbackResult.Diagnostics)
		{
			AddError(FString::Printf(TEXT("%s rollback diagnostic %s: %s"),
				Label, *Diagnostic.Code, *Diagnostic.Message));
		}
		LocalPassed &= TestTrue(*(FString(Label) + TEXT(" rolls back through Atomic")),
			RollbackResult.bValid);
		LocalPassed &= RequireObservation(*this, FString(Label) + TEXT(" restores exact preimage"), Before, *Blackboard);
		LocalPassed &= RequireObservation(*this,
			FString(Label) + TEXT(" restores child derived state"), ChildBefore, *Child);
		CollectGarbage(RF_NoFlags);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" releases staged KeyType after rollback")),
			CountDirectBlackboardKeyTypes(*Blackboard), DirectKeyTypesBefore);
		return LocalPassed;
	};

	Passed &= RunMaterializedFailure(TEXT("entry add staging failure"), 11,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("entry add post-staging failure"), 12,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("entry add configuration failure"), 13,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("entry add pre-publication failure"), 14,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("entry add publication rejection"), 15,
		TEXT("AIProvider.AuthorityUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("entry add post-publication failure"), 16,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunMaterializedFailure(TEXT("entry add derived refresh failure"), 17,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunMaterializedFailure(TEXT("entry add readback failure"), 18,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunMaterializedFailure(TEXT("entry add ChangedSet mismatch"), 19,
		TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"));
	FAssetDocumentRuntimeValueDto InvalidProperties = MakeEntryAddV2Entry(TEXT("InvalidProperties"));
	InvalidProperties.Elements[3] = ObjectValue({TEXT("Int32Value")}, {IntegerValue(41)});
	Passed &= RunMaterializedFailure(TEXT("entry add invalid complete properties"), 13,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), MoveTemp(InvalidProperties));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReplaceClassSuccessTest,
	"UEAssetDoc.AIProvider.BlackboardV2.ReplaceClass.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReplaceClassSuccessTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: replace one owned-local slot with a distinct exact
	//   KeyType class and complete entry state while preserving name/order.
	// - Candidate technique: state-transition plus exact output/rollback oracle.
	// - Risk or gap: UPDATE dispatch, append/reorder, or loss of the old KeyType
	//   can all look superficially successful.
	// - Test: replace the middle entry, inspect child derived state, then
	//   roll the completed operation back.
	// - Expected observable outcome: exact full DTO, five actual ChangedSet paths,
	//   stable slot/order, new class forward and old exact instance after rollback.
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ReplaceClassSuccess_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("replace-class success Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("replace-class success child fixture exists"), Child)) return false;
	TStrongObjectPtr<UBlackboardData> BlackboardGuard(Blackboard);
	TStrongObjectPtr<UBlackboardData> ChildGuard(Child);
	AddExistingKey(*Blackboard, TEXT("First"), 1);
	AddExistingKey(*Blackboard, TEXT("Target"), 7);
	AddExistingKey(*Blackboard, TEXT("Last"), 2);
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Blackboard->GetOutermost()->SetDirtyFlag(false);

	const int32 TargetIndex = Blackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
	{
		return Entry.EntryName == TEXT("Target");
	});
	if (!TestTrue(TEXT("replace-class target exact slot exists"), TargetIndex != INDEX_NONE)) return false;
	UBlackboardKeyType* const OldKeyType = Blackboard->Keys[TargetIndex].KeyType;
	const FBlackboardObservation Before(*Blackboard);
	const FBlackboardObservation ChildBefore(*Child);
	CollectGarbage(RF_NoFlags);
	const int32 DirectKeyTypesBefore = CountDirectBlackboardKeyTypes(*Blackboard);
	FBlackboardLifecycleTransaction Transaction;
	FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
		*this, *Blackboard, Transaction, Handles);
	if (!Handle.IsSet()) return false;
	const FAssetDocumentRuntimeValueDto ExpectedEntry =
		MakeEntryReplaceClassV2IntEntry(TEXT("Target"));
	const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryReplaceClassV2Invocation(
			Handle.GetValue(), *Blackboard, TEXT("Target"), ExpectedEntry),
		Handles, &Transaction);
	const FAssetDocumentRuntimeValueDto ExpectedOutput = ObjectValue(
		{TEXT("entry"), TEXT("ordered_local_key_names")},
		{ExpectedEntry, ArrayValue({StringValue(TEXT("First")),
			StringValue(TEXT("Target")), StringValue(TEXT("Last"))})});
	const UBlackboardKeyType_Int* NewKeyType =
		TargetIndex < Blackboard->Keys.Num()
			? Cast<UBlackboardKeyType_Int>(Blackboard->Keys[TargetIndex].KeyType)
			: nullptr;
	bool Passed = RequireSuccess(*this, TEXT("replace-class distinct class"), Outcome)
		&& TestTrue(TEXT("replace-class returns exact full entry and ordered names"),
			RuntimeValueEquals(Outcome.Value, ExpectedOutput))
		&& TestEqual(TEXT("replace-class preserves exact physical slot name"),
			Blackboard->Keys[TargetIndex].EntryName, FName(TEXT("Target")))
		&& TestNotNull(TEXT("replace-class materializes exact new KeyType class"), NewKeyType)
		&& (NewKeyType == nullptr || TestEqual(TEXT("replace-class configures complete new state"),
			NewKeyType->DefaultValue, 73))
		&& TestNotEqual(TEXT("replace-class publishes a distinct KeyType instance"),
			Blackboard->Keys[TargetIndex].KeyType.Get(), OldKeyType)
		&& TestEqual(TEXT("replace-class captures before one complete footprint"),
			Transaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")})
		&& TestEqual(TEXT("replace-class enlists exactly one footprint"),
			Transaction.EnlistedFootprint.Num(), 1)
		&& TestEqual(TEXT("replace-class reports only five actual entry-field changes"),
			Outcome.ChangedSet.Num(), 5)
		&& TestEqual(TEXT("replace-class retains old and staged KeyTypes until terminal"),
			CountDirectBlackboardKeyTypes(*Blackboard), DirectKeyTypesBefore + 1)
		&& TestEqual(TEXT("replace-class refreshes exact child KeyType"),
			Child->GetKeyType(Child->GetKeyID(TEXT("Target"))).Get(),
			UBlackboardKeyType_Int::StaticClass())
		&& TestTrue(TEXT("replace-class refreshes child synchronized-key state"),
			Child->HasSynchronizedKeys()
				&& Child->IsKeyInstanceSynced(Child->GetKeyID(TEXT("Target"))));
	if (Transaction.EnlistedFootprint.Num() == 1)
	{
		Passed &= TestEqual(TEXT("replace-class uses exact LocalKeysAggregate component"),
			Transaction.EnlistedFootprint[0].RollbackComponentId,
			FString(TEXT("assetdocument.ai.blackboard.entry.replace_class.v2")));
	}
	const TArray<FString> ExpectedChangedPaths = {
		FString::Printf(TEXT("asset:%s/Keys[name=Target].KeyTypeClass"), *Blackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].KeyTypeProperties"), *Blackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].Description"), *Blackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].Category"), *Blackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].bInstanceSynced"), *Blackboard->GetPathName())};
	if (Outcome.ChangedSet.Num() == ExpectedChangedPaths.Num())
	{
		for (int32 Index = 0; Index < ExpectedChangedPaths.Num(); ++Index)
		{
			Passed &= TestEqual(TEXT("replace-class ChangedSet path is exact"),
				Outcome.ChangedSet[Index].Path, ExpectedChangedPaths[Index]);
			Passed &= TestEqual(TEXT("replace-class ChangedSet authority is exact"),
				Outcome.ChangedSet[Index].AuthorityId,
				FString(TEXT("ai.blackboard.entry.replace_class.v2")));
			Passed &= TestEqual(TEXT("replace-class ChangedSet object is exact"),
				Outcome.ChangedSet[Index].StableObjectId, Blackboard->GetPathName());
			Passed &= TestEqual(TEXT("replace-class ChangedSet kind is Update"),
				Outcome.ChangedSet[Index].ChangeKind, FString(TEXT("Update")));
		}
	}
	const FAssetDocumentContractValidationResult RollbackResult =
		Transaction.Rollback(TEXT("Test.EntryReplaceClassLateRollback"));
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : RollbackResult.Diagnostics)
	{
		AddError(FString::Printf(TEXT("replace-class late rollback diagnostic %s: %s"),
			*Diagnostic.Code, *Diagnostic.Message));
	}
	Passed &= TestTrue(TEXT("replace-class late rollback succeeds"), RollbackResult.bValid);
	Passed &= RequireObservation(*this, TEXT("replace-class restores exact local preimage"),
		Before, *Blackboard);
	Passed &= RequireObservation(*this, TEXT("replace-class restores exact child derived state"),
		ChildBefore, *Child);
	Passed &= TestEqual(TEXT("replace-class restores old exact KeyType instance"),
		Blackboard->Keys[TargetIndex].KeyType.Get(), OldKeyType);
	CollectGarbage(RF_NoFlags);
	Passed &= TestEqual(TEXT("replace-class rollback releases staged KeyType"),
		CountDirectBlackboardKeyTypes(*Blackboard), DirectKeyTypesBefore);

	// StructCanonicalBothDirections (SC-REP-001).
	// Test design:
	// - Behavior under test: replacing an owned local non-Struct KeyType with the
	//   exact stock Struct KeyType writes the admitted nested DefaultValue and
	//   preserves the complete replace/rollback contract.
	// - Candidate technique: state-transition testing with literal canonical DTO
	//   and native-value oracles.
	// - Risk or gap: a refusal, generic property write, incomplete entry, or a
	//   rollback that restores only the slot can look like a valid replacement.
	// - Test: non-Struct -> exact Struct with Count=41 and Label=desired.
	// - Expected observable outcome: success, exact entry/order, one action and
	//   footprint, staged Struct ownership/value, child refresh and full rollback.
	UPackage* ToStructPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ReplaceClassToStruct_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* ToStructBlackboard = NewObject<UBlackboardData>(
		ToStructPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* ToStructChild = NewObject<UBlackboardData>(
		ToStructPackage, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("replace-class Struct-forward Blackboard fixture exists"), ToStructBlackboard)
		|| !TestNotNull(TEXT("replace-class Struct-forward child fixture exists"), ToStructChild)) return false;
	TStrongObjectPtr<UBlackboardData> ToStructBlackboardGuard(ToStructBlackboard);
	TStrongObjectPtr<UBlackboardData> ToStructChildGuard(ToStructChild);
	AddExistingKey(*ToStructBlackboard, TEXT("First"), 1);
	AddExistingKey(*ToStructBlackboard, TEXT("Target"), 7);
	AddExistingKey(*ToStructBlackboard, TEXT("Last"), 2);
	ToStructBlackboard->UpdateParentKeys();
	ToStructBlackboard->UpdateKeyIDs();
	ToStructBlackboard->UpdateIfHasSynchronizedKeys();
	ToStructChild->Parent = ToStructBlackboard;
	ToStructChild->UpdateParentKeys();
	ToStructChild->UpdateKeyIDs();
	ToStructChild->UpdateIfHasSynchronizedKeys();
	ToStructBlackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	ToStructBlackboard->GetOutermost()->SetDirtyFlag(false);
	const int32 ToStructTargetIndex = ToStructBlackboard->Keys.IndexOfByPredicate(
		[](const FBlackboardEntry& Entry) { return Entry.EntryName == TEXT("Target"); });
	if (!TestTrue(TEXT("replace-class Struct-forward target exact slot exists"),
		ToStructTargetIndex != INDEX_NONE)) return false;
	UBlackboardKeyType* const ToStructOldKeyType = ToStructBlackboard->Keys[ToStructTargetIndex].KeyType;
	const FBlackboardObservation ToStructBefore(*ToStructBlackboard);
	const FBlackboardObservation ToStructChildBefore(*ToStructChild);
	CollectGarbage(RF_NoFlags);
	const int32 ToStructDirectKeyTypesBefore = CountDirectBlackboardKeyTypes(*ToStructBlackboard);
	const FAssetDocumentRuntimeValueDto DesiredStructDefault = ObjectValue(
		{TEXT("Properties"), TEXT("Struct")},
		{ObjectValue({TEXT("Count"), TEXT("Label")},
			{IntegerValue(41), StringValue(TEXT("desired"))}),
			StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())});
	const FAssetDocumentRuntimeValueDto ToStructEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("StructReplacement")), StringValue(TEXT("Canonical Struct replacement")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {DesiredStructDefault}),
			StringValue(TEXT("Target")), BooleanValue(true)});
	const FAssetDocumentRuntimeValueDto ToStructOutput = ObjectValue(
		{TEXT("entry"), TEXT("ordered_local_key_names")},
		{ToStructEntry, ArrayValue({StringValue(TEXT("First")), StringValue(TEXT("Target")),
			StringValue(TEXT("Last"))})});
	FBlackboardLifecycleTransaction ToStructTransaction;
	FBlackboardLifecycleHandleResolver ToStructHandles(ToStructTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> ToStructHandle = ResolveAIMutableAsset(
		*this, *ToStructBlackboard, ToStructTransaction, ToStructHandles);
	if (!ToStructHandle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome ToStructOutcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryReplaceClassV2Invocation(ToStructHandle.GetValue(), *ToStructBlackboard,
			TEXT("Target"), ToStructEntry), ToStructHandles, &ToStructTransaction);
	const UBlackboardKeyType_Struct* ToStructNewKeyType = ToStructTargetIndex < ToStructBlackboard->Keys.Num()
		? Cast<UBlackboardKeyType_Struct>(ToStructBlackboard->Keys[ToStructTargetIndex].KeyType) : nullptr;
	const FAssetDocumentAIBlackboardCanonicalStructNested* ToStructNativeValue = ToStructNewKeyType != nullptr
		? ToStructNewKeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>() : nullptr;
	bool ToStructPassed = RequireSuccess(*this, TEXT("replace-class Struct-forward succeeds"), ToStructOutcome)
		&& TestTrue(TEXT("replace-class Struct-forward returns exact complete canonical entry/order"),
			RuntimeValueEquals(ToStructOutcome.Value, ToStructOutput))
		&& TestEqual(TEXT("replace-class Struct-forward preserves exact slot/name"),
			ToStructBlackboard->Keys[ToStructTargetIndex].EntryName, FName(TEXT("Target")))
		&& TestNotNull(TEXT("replace-class Struct-forward stages exact stock Struct class"), ToStructNewKeyType)
		&& (ToStructNewKeyType == nullptr || TestEqual(TEXT("replace-class Struct-forward staged Struct outer is Blackboard"),
			ToStructNewKeyType->GetOuter(), static_cast<UObject*>(ToStructBlackboard)))
		&& TestTrue(TEXT("replace-class Struct-forward writes exact native InstancedStruct value"),
			ToStructNativeValue != nullptr && ToStructNativeValue->Count == 41
				&& ToStructNativeValue->Label == TEXT("desired"))
		&& TestEqual(TEXT("replace-class Struct-forward captures one action and footprint"),
			ToStructTransaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")})
		&& TestEqual(TEXT("replace-class Struct-forward enlists one footprint"),
			ToStructTransaction.EnlistedFootprint.Num(), 1)
		&& TestEqual(TEXT("replace-class Struct-forward reports five actual changes"),
			ToStructOutcome.ChangedSet.Num(), 5)
		&& TestEqual(TEXT("replace-class Struct-forward refreshes child exact KeyType"),
			ToStructChild->GetKeyType(ToStructChild->GetKeyID(TEXT("Target"))).Get(),
			UBlackboardKeyType_Struct::StaticClass())
		&& TestTrue(TEXT("replace-class Struct-forward refreshes child synchronized state"),
			ToStructChild->HasSynchronizedKeys()
				&& ToStructChild->IsKeyInstanceSynced(ToStructChild->GetKeyID(TEXT("Target"))));
	if (ToStructTransaction.EnlistedFootprint.Num() == 1)
	{
		ToStructPassed &= TestEqual(TEXT("replace-class Struct-forward footprint authority is exact"),
			ToStructTransaction.EnlistedFootprint[0].RollbackComponentId,
			FString(TEXT("assetdocument.ai.blackboard.entry.replace_class.v2")));
	}
	const TArray<FString> ToStructChangedPaths = {
		FString::Printf(TEXT("asset:%s/Keys[name=Target].KeyTypeClass"), *ToStructBlackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].KeyTypeProperties"), *ToStructBlackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].Description"), *ToStructBlackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].Category"), *ToStructBlackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].bInstanceSynced"), *ToStructBlackboard->GetPathName())};
	if (ToStructOutcome.ChangedSet.Num() == ToStructChangedPaths.Num())
	{
		for (int32 Index = 0; Index < ToStructChangedPaths.Num(); ++Index)
		{
			ToStructPassed &= TestEqual(TEXT("replace-class Struct-forward ChangedSet path is exact"),
				ToStructOutcome.ChangedSet[Index].Path, ToStructChangedPaths[Index]);
			ToStructPassed &= TestEqual(TEXT("replace-class Struct-forward ChangedSet authority is exact"),
				ToStructOutcome.ChangedSet[Index].AuthorityId,
				FString(TEXT("ai.blackboard.entry.replace_class.v2")));
			ToStructPassed &= TestEqual(TEXT("replace-class Struct-forward ChangedSet object is exact"),
				ToStructOutcome.ChangedSet[Index].StableObjectId, ToStructBlackboard->GetPathName());
			ToStructPassed &= TestEqual(TEXT("replace-class Struct-forward ChangedSet kind is Update"),
				ToStructOutcome.ChangedSet[Index].ChangeKind, FString(TEXT("Update")));
		}
	}
	const FAssetDocumentContractValidationResult ToStructRollback = ToStructTransaction.Rollback(
		TEXT("Test.EntryReplaceClassStructForwardLateRollback"));
	ToStructPassed &= TestTrue(TEXT("replace-class Struct-forward late rollback succeeds"), ToStructRollback.bValid);
	ToStructPassed &= RequireObservation(*this, TEXT("replace-class Struct-forward rollback restores local preimage"),
		ToStructBefore, *ToStructBlackboard);
	ToStructPassed &= RequireObservation(*this, TEXT("replace-class Struct-forward rollback restores child derived state"),
		ToStructChildBefore, *ToStructChild);
	ToStructPassed &= TestEqual(TEXT("replace-class Struct-forward rollback restores old exact pointer"),
		ToStructBlackboard->Keys[ToStructTargetIndex].KeyType.Get(), ToStructOldKeyType);
	CollectGarbage(RF_NoFlags);
	ToStructPassed &= TestEqual(TEXT("replace-class Struct-forward rollback releases staged Struct"),
		CountDirectBlackboardKeyTypes(*ToStructBlackboard), ToStructDirectKeyTypesBefore);
	Passed &= ToStructPassed;

	// Test design:
	// - Behavior under test: replacing an owned exact stock Struct with Int uses
	//   released READ for the old value and never enters canonical WRITE.
	// - Candidate technique: directional state-transition partition.
	// - Risk or gap: forward-write routing can accidentally be used for the old
	//   Struct pre-state, or rollback can lose its native FInstancedStruct value.
	// - Test: exact Struct -> non-Struct with independent fixture.
	// - Expected observable outcome: success, exact entry/order and rollback of
	//   the old Struct pointer/value while both WRITE audits remain NotEntered.
	// - Expected failure reason: a regression would enter canonical WRITE or
	//   drop the old Struct native preimage instead of consuming READ only.
	UPackage* FromStructPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ReplaceClassFromStruct_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* FromStructBlackboard = NewObject<UBlackboardData>(
		FromStructPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* FromStructChild = NewObject<UBlackboardData>(
		FromStructPackage, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("replace-class Struct-reverse Blackboard fixture exists"), FromStructBlackboard)
		|| !TestNotNull(TEXT("replace-class Struct-reverse child fixture exists"), FromStructChild)) return false;
	TStrongObjectPtr<UBlackboardData> FromStructBlackboardGuard(FromStructBlackboard);
	TStrongObjectPtr<UBlackboardData> FromStructChildGuard(FromStructChild);
	AddExistingKey(*FromStructBlackboard, TEXT("First"), 1);
	UBlackboardKeyType_Struct* FromStructOldKeyType = NewObject<UBlackboardKeyType_Struct>(
		FromStructBlackboard, TEXT("TargetStructKeyType"), RF_Transactional);
	FromStructOldKeyType->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	FAssetDocumentAIBlackboardCanonicalStructNested& FromStructOldValue =
		FromStructOldKeyType->DefaultValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>();
	FromStructOldValue.Count = 41;
	FromStructOldValue.Label = TEXT("desired");
	FBlackboardEntry FromStructEntry;
	FromStructEntry.EntryName = TEXT("Target");
	FromStructEntry.EntryDescription = TEXT("Struct before replacement");
	FromStructEntry.EntryCategory = TEXT("StructBefore");
	FromStructEntry.KeyType = FromStructOldKeyType;
	FromStructBlackboard->Keys.Add(FromStructEntry);
	AddExistingKey(*FromStructBlackboard, TEXT("Last"), 2);
	FromStructBlackboard->UpdateParentKeys();
	FromStructBlackboard->UpdateKeyIDs();
	FromStructBlackboard->UpdateIfHasSynchronizedKeys();
	FromStructChild->Parent = FromStructBlackboard;
	FromStructChild->UpdateParentKeys();
	FromStructChild->UpdateKeyIDs();
	FromStructChild->UpdateIfHasSynchronizedKeys();
	FromStructBlackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	FromStructBlackboard->GetOutermost()->SetDirtyFlag(false);
	const int32 FromStructTargetIndex = FromStructBlackboard->Keys.IndexOfByPredicate(
		[](const FBlackboardEntry& Entry) { return Entry.EntryName == TEXT("Target"); });
	if (!TestTrue(TEXT("replace-class Struct-reverse target exact slot exists"),
		FromStructTargetIndex != INDEX_NONE)) return false;
	const FBlackboardObservation FromStructBefore(*FromStructBlackboard);
	const FBlackboardObservation FromStructChildBefore(*FromStructChild);
	CollectGarbage(RF_NoFlags);
	const int32 FromStructDirectKeyTypesBefore = CountDirectBlackboardKeyTypes(*FromStructBlackboard);
	const FAssetDocumentRuntimeValueDto FromStructToIntEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("IntReplacement")), StringValue(TEXT("Replaced from canonical Struct")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Int::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {IntegerValue(29)}),
			StringValue(TEXT("Target")), BooleanValue(true)});
	const FAssetDocumentRuntimeValueDto FromStructToIntOutput = ObjectValue(
		{TEXT("entry"), TEXT("ordered_local_key_names")},
		{FromStructToIntEntry, ArrayValue({StringValue(TEXT("First")), StringValue(TEXT("Target")),
			StringValue(TEXT("Last"))})});
	FBlackboardLifecycleTransaction FromStructTransaction;
	FBlackboardLifecycleHandleResolver FromStructHandles(FromStructTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> FromStructHandle = ResolveAIMutableAsset(
		*this, *FromStructBlackboard, FromStructTransaction, FromStructHandles);
	if (!FromStructHandle.IsSet()) return false;
	AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ResetPrepareAudit();
	AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ResetOuterPreflightAudit();
	const FAssetDocumentPrimitiveOutcome FromStructOutcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryReplaceClassV2Invocation(FromStructHandle.GetValue(), *FromStructBlackboard,
			TEXT("Target"), FromStructToIntEntry), FromStructHandles, &FromStructTransaction);
	const UBlackboardKeyType_Int* FromStructNewKeyType = FromStructTargetIndex < FromStructBlackboard->Keys.Num()
		? Cast<UBlackboardKeyType_Int>(FromStructBlackboard->Keys[FromStructTargetIndex].KeyType) : nullptr;
	bool FromStructPassed = RequireSuccess(*this, TEXT("replace-class Struct-reverse succeeds"), FromStructOutcome)
		&& TestTrue(TEXT("replace-class Struct-reverse returns exact complete canonical entry/order"),
			RuntimeValueEquals(FromStructOutcome.Value, FromStructToIntOutput))
		&& TestEqual(TEXT("replace-class Struct-reverse preserves exact slot/name"),
			FromStructBlackboard->Keys[FromStructTargetIndex].EntryName, FName(TEXT("Target")))
		&& TestNotNull(TEXT("replace-class Struct-reverse stages exact Int class"), FromStructNewKeyType)
		&& (FromStructNewKeyType == nullptr || TestEqual(TEXT("replace-class Struct-reverse staged Int outer is Blackboard"),
			FromStructNewKeyType->GetOuter(), static_cast<UObject*>(FromStructBlackboard)))
		&& (FromStructNewKeyType == nullptr || TestEqual(TEXT("replace-class Struct-reverse writes exact Int native value"),
			FromStructNewKeyType->DefaultValue, 29))
		&& TestEqual(TEXT("replace-class Struct-reverse canonical WRITE prepare is not entered"),
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::GetPrepareAuditTerminal(),
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EPrepareAuditTerminal::NotEntered)
		&& TestEqual(TEXT("replace-class Struct-reverse canonical WRITE lifecycle preflight is not entered"),
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::GetOuterPreflightAuditTerminal(),
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal::NotEntered)
		&& TestEqual(TEXT("replace-class Struct-reverse captures one action and footprint"),
			FromStructTransaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")})
		&& TestEqual(TEXT("replace-class Struct-reverse enlists one footprint"),
			FromStructTransaction.EnlistedFootprint.Num(), 1)
		&& TestEqual(TEXT("replace-class Struct-reverse reports five actual changes"),
			FromStructOutcome.ChangedSet.Num(), 5)
		&& TestEqual(TEXT("replace-class Struct-reverse refreshes child exact KeyType"),
			FromStructChild->GetKeyType(FromStructChild->GetKeyID(TEXT("Target"))).Get(),
			UBlackboardKeyType_Int::StaticClass())
		&& TestTrue(TEXT("replace-class Struct-reverse refreshes child synchronized state"),
			FromStructChild->HasSynchronizedKeys()
				&& FromStructChild->IsKeyInstanceSynced(FromStructChild->GetKeyID(TEXT("Target"))));
	if (FromStructTransaction.EnlistedFootprint.Num() == 1)
	{
		FromStructPassed &= TestEqual(TEXT("replace-class Struct-reverse footprint authority is exact"),
			FromStructTransaction.EnlistedFootprint[0].RollbackComponentId,
			FString(TEXT("assetdocument.ai.blackboard.entry.replace_class.v2")));
	}
	const TArray<FString> FromStructChangedPaths = {
		FString::Printf(TEXT("asset:%s/Keys[name=Target].KeyTypeClass"), *FromStructBlackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].KeyTypeProperties"), *FromStructBlackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].Description"), *FromStructBlackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].Category"), *FromStructBlackboard->GetPathName()),
		FString::Printf(TEXT("asset:%s/Keys[name=Target].bInstanceSynced"), *FromStructBlackboard->GetPathName())};
	if (FromStructOutcome.ChangedSet.Num() == FromStructChangedPaths.Num())
	{
		for (int32 Index = 0; Index < FromStructChangedPaths.Num(); ++Index)
		{
			FromStructPassed &= TestEqual(TEXT("replace-class Struct-reverse ChangedSet path is exact"),
				FromStructOutcome.ChangedSet[Index].Path, FromStructChangedPaths[Index]);
			FromStructPassed &= TestEqual(TEXT("replace-class Struct-reverse ChangedSet authority is exact"),
				FromStructOutcome.ChangedSet[Index].AuthorityId,
				FString(TEXT("ai.blackboard.entry.replace_class.v2")));
			FromStructPassed &= TestEqual(TEXT("replace-class Struct-reverse ChangedSet object is exact"),
				FromStructOutcome.ChangedSet[Index].StableObjectId, FromStructBlackboard->GetPathName());
			FromStructPassed &= TestEqual(TEXT("replace-class Struct-reverse ChangedSet kind is Update"),
				FromStructOutcome.ChangedSet[Index].ChangeKind, FString(TEXT("Update")));
		}
	}
	const FAssetDocumentContractValidationResult FromStructRollback = FromStructTransaction.Rollback(
		TEXT("Test.EntryReplaceClassStructReverseLateRollback"));
	FromStructPassed &= TestTrue(TEXT("replace-class Struct-reverse late rollback succeeds"), FromStructRollback.bValid);
	FromStructPassed &= RequireObservation(*this, TEXT("replace-class Struct-reverse rollback restores local preimage"),
		FromStructBefore, *FromStructBlackboard);
	FromStructPassed &= RequireObservation(*this, TEXT("replace-class Struct-reverse rollback restores child derived state"),
		FromStructChildBefore, *FromStructChild);
	const UBlackboardKeyType_Struct* FromStructRolledBackKeyType =
		Cast<UBlackboardKeyType_Struct>(FromStructBlackboard->Keys[FromStructTargetIndex].KeyType);
	const FAssetDocumentAIBlackboardCanonicalStructNested* FromStructRolledBackValue =
		FromStructRolledBackKeyType != nullptr
			? FromStructRolledBackKeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>() : nullptr;
	FromStructPassed &= TestEqual(TEXT("replace-class Struct-reverse rollback restores old exact Struct pointer"),
		static_cast<const UBlackboardKeyType*>(FromStructRolledBackKeyType),
		static_cast<const UBlackboardKeyType*>(FromStructOldKeyType));
	FromStructPassed &= TestTrue(TEXT("replace-class Struct-reverse rollback restores old native Struct value"),
		FromStructRolledBackValue != nullptr && FromStructRolledBackValue->Count == 41
			&& FromStructRolledBackValue->Label == TEXT("desired"));
	CollectGarbage(RF_NoFlags);
	FromStructPassed &= TestEqual(TEXT("replace-class Struct-reverse rollback releases staged Int"),
		CountDirectBlackboardKeyTypes(*FromStructBlackboard), FromStructDirectKeyTypesBefore);
	Passed &= FromStructPassed;

	FBlackboardLifecycleTransaction MinimalTransaction;
	FBlackboardLifecycleHandleResolver MinimalHandles(MinimalTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> MinimalHandle = ResolveAIMutableAsset(
		*this, *Blackboard, MinimalTransaction, MinimalHandles);
	if (!MinimalHandle.IsSet()) return false;
	FAssetDocumentRuntimeValueDto MinimalEntry =
		MakeEntryReplaceClassV2IntEntry(TEXT("Target"), 91);
	MinimalEntry.Elements[0] = StringValue(FString());
	MinimalEntry.Elements[1] = StringValue(FString());
	MinimalEntry.Elements[5] = BooleanValue(false);
	const FAssetDocumentPrimitiveOutcome MinimalOutcome =
		CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryReplaceClassV2Invocation(
				MinimalHandle.GetValue(), *Blackboard, TEXT("Target"), MinimalEntry),
			MinimalHandles, &MinimalTransaction);
	Passed &= RequireSuccess(*this, TEXT("replace-class minimal actual differences"), MinimalOutcome);
	Passed &= TestEqual(TEXT("replace-class omits unchanged metadata from ChangedSet"),
		MinimalOutcome.ChangedSet.Num(), 2);
	if (MinimalOutcome.ChangedSet.Num() == 2)
	{
		Passed &= TestEqual(TEXT("replace-class minimal ChangedSet starts with class"),
			MinimalOutcome.ChangedSet[0].Path,
			FString::Printf(TEXT("asset:%s/Keys[name=Target].KeyTypeClass"),
				*Blackboard->GetPathName()));
		Passed &= TestEqual(TEXT("replace-class minimal ChangedSet ends with properties"),
			MinimalOutcome.ChangedSet[1].Path,
			FString::Printf(TEXT("asset:%s/Keys[name=Target].KeyTypeProperties"),
				*Blackboard->GetPathName()));
	}
	const FAssetDocumentContractValidationResult MinimalRollbackResult =
		MinimalTransaction.Rollback(TEXT("Test.EntryReplaceClassMinimalRollback"));
	Passed &= TestTrue(TEXT("replace-class minimal-difference rollback succeeds"),
		MinimalRollbackResult.bValid);
	Passed &= RequireObservation(*this, TEXT("replace-class minimal rollback restores preimage"),
		Before, *Blackboard);
	Passed &= RequireObservation(*this, TEXT("replace-class minimal rollback restores child"),
		ChildBefore, *Child);
	Passed &= TestEqual(TEXT("replace-class minimal rollback restores old exact KeyType instance"),
		Blackboard->Keys[TargetIndex].KeyType.Get(), OldKeyType);
	CollectGarbage(RF_NoFlags);
	Passed &= TestEqual(TEXT("replace-class minimal rollback releases staged KeyType"),
		CountDirectBlackboardKeyTypes(*Blackboard), DirectKeyTypesBefore);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReplaceClassSaveReloadTest,
	"UEAssetDoc.AIProvider.BlackboardV2.ReplaceClass.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReplaceClassSaveReloadTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentAIBlackboardReplaceClass/BlackboardReplaceClassReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PackageFileName, false, true);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("replace-class save-reload Blackboard exists"), Blackboard)) return false;
	AddExistingKey(*Blackboard, TEXT("PersistedInt"), 7);
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Package->SetDirtyFlag(false);
	const FString KeyName = TEXT("PersistedInt");
	const int32 DefaultValue = 73;
	const FAssetDocumentRuntimeValueDto ExpectedEntry =
		MakeEntryReplaceClassV2IntEntry(KeyName, DefaultValue);
	bool Passed = true;
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryReplaceClassV2Invocation(
				Handle.GetValue(), *Blackboard, KeyName, ExpectedEntry),
			Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("replace-class before save-reload"), Outcome);
		Passed &= TestTrue(TEXT("replace-class save-reload output contains canonical entry"),
			Outcome.Value.Elements.Num() == 2
				&& RuntimeValueEquals(Outcome.Value.Elements[0], ExpectedEntry));
		FBlackboardKeySelector Selector;
		Selector.SelectedKeyName = FName(KeyName);
		Selector.ResolveSelectedKey(*Blackboard);
		Passed &= TestEqual(TEXT("replace-class resolves selector Key-ID"),
			Selector.GetSelectedKeyID(), Blackboard->GetKeyID(FName(KeyName)));
		Passed &= TestEqual(TEXT("replace-class resolves selector type"),
			Selector.SelectedKeyType.Get(), UBlackboardKeyType_Int::StaticClass());
		UWorld* TestWorld = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
		Passed &= TestNotNull(TEXT("replace-class value-or-key has an Editor world"), TestWorld);
		UBlackboardComponent* BlackboardComponent =
			TestWorld != nullptr ? NewObject<UBlackboardComponent>(TestWorld) : nullptr;
		Passed &= TestTrue(TEXT("replace-class initializes value-or-key Blackboard"),
			BlackboardComponent != nullptr
				&& BlackboardComponent->InitializeBlackboard(*Blackboard));
		if (BlackboardComponent != nullptr)
		{
			FValueOrBBKey_Int32 ValueOrKey(-1);
			ValueOrKey.SetKey(FName(KeyName));
			Passed &= TestEqual(TEXT("replace-class value-or-key observes configured default"),
				ValueOrKey.GetValue(*BlackboardComponent), DefaultValue);
		}
		Passed &= TestTrue(TEXT("replace-class transaction becomes commit-ready"),
			Transaction.MarkReadyToCommit().bValid);
		Passed &= TestTrue(TEXT("replace-class transaction commits before persistence"),
			Transaction.Commit(MakeStructuralCompletionReceipt(Transaction)).bValid);
	}
	Package->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	Passed &= TestTrue(TEXT("replace-class result saves through external lifecycle"),
		UPackage::SavePackage(Package, Blackboard, *PackageFileName, SaveArgs));
	if (!Passed) return false;
	Passed &= TestTrue(TEXT("replace-class package unloads before canonical reload"),
		UPackageTools::UnloadPackages({Package}));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	UBlackboardData* Reloaded = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("replace-class Blackboard fresh-reloads"), Reloaded)) return false;
	const FBlackboardEntry* ReloadedEntry = Reloaded->Keys.FindByPredicate(
		[&KeyName](const FBlackboardEntry& Entry)
		{
			return Entry.EntryName.ToString() == KeyName;
		});
	if (!TestNotNull(TEXT("fresh reload retains replaced entry"), ReloadedEntry)) return false;
	const UBlackboardKeyType_Int* ReloadedKeyType =
		Cast<UBlackboardKeyType_Int>(ReloadedEntry->KeyType);
	Passed &= TestNotNull(TEXT("fresh reload retains replacement KeyType class"), ReloadedKeyType);
	Passed &= TestEqual(TEXT("fresh reload retains replacement KeyType state"),
		ReloadedKeyType != nullptr ? ReloadedKeyType->DefaultValue : INDEX_NONE, DefaultValue);
	Passed &= TestEqual(TEXT("fresh reload retains replacement Description"),
		ReloadedEntry->EntryDescription, FString(TEXT("Replaced class")));
	Passed &= TestEqual(TEXT("fresh reload retains replacement Category"),
		ReloadedEntry->EntryCategory, FName(TEXT("Replacement")));
	Passed &= TestTrue(TEXT("fresh reload retains replacement synchronized state"),
		ReloadedEntry->bInstanceSynced != 0 && Reloaded->HasSynchronizedKeys());
	FBlackboardLifecycleTransaction ReadbackTransaction;
	FBlackboardLifecycleHandleResolver ReadbackHandles(ReadbackTransaction.GetMutationToken());
	const FAssetDocumentPrimitiveOutcome Readback = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryReadbackInvocation(*Reloaded, ReadbackTransaction.GetMutationToken(), KeyName),
		ReadbackHandles, nullptr);
	Passed &= RequireSuccess(*this, TEXT("replace-class fresh reload canonical readback"), Readback);
	Passed &= TestTrue(TEXT("replace-class fresh reload equals successful canonical entry"),
		Readback.Value.ObjectKeys == TArray<FString>{TEXT("entry")}
			&& Readback.Value.Elements.Num() == 1
			&& RuntimeValueEquals(Readback.Value.Elements[0], ExpectedEntry));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ReplaceClassFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.ReplaceClass.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ReplaceClassFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ReplaceClassFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Parent = NewObject<UBlackboardData>(
		Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("replace-class failure Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("replace-class failure parent fixture exists"), Parent)
		|| !TestNotNull(TEXT("replace-class failure child fixture exists"), Child)) return false;
	TStrongObjectPtr<UBlackboardData> BlackboardGuard(Blackboard);
	TStrongObjectPtr<UBlackboardData> ParentGuard(Parent);
	TStrongObjectPtr<UBlackboardData> ChildGuard(Child);
	AddExistingKey(*Blackboard, TEXT("Existing"), 7);
	AddExistingKey(*Parent, TEXT("Inherited"), 9);
	Parent->UpdateParentKeys();
	Parent->UpdateKeyIDs();
	Parent->UpdateIfHasSynchronizedKeys();
	Blackboard->Parent = Parent;
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Parent->PropagateKeyChangesToDerivedBlackboardAssets();
	Blackboard->GetOutermost()->SetDirtyFlag(false);

	auto RunPreflightFailure = [&](const TCHAR* Label, const FString& KeyName,
		FAssetDocumentRuntimeValueDto Entry, const TCHAR* ExpectedCode,
		TFunction<void(FAssetDocumentPrimitiveInvocation&)> Mutate = {})
	{
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveInvocation Invocation = MakeEntryReplaceClassV2Invocation(
			Handle.GetValue(), *Blackboard, KeyName, MoveTemp(Entry));
		if (Mutate) Mutate(Invocation);
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			Invocation, Handles, &Transaction);
		const bool LocalPassed = RequireExactFailureTerminal(*this, Label, Outcome, Before,
			*Blackboard, Transaction, ExpectedCode)
			&& TestEqual(*(FString(Label) + TEXT(" captures no action")),
				Transaction.ActionEnlistCount, 0)
			&& TestEqual(*(FString(Label) + TEXT(" enlists no footprint")),
				Transaction.FootprintEnlistCount, 0);
		if (!LocalPassed && Transaction.ActionEnlistCount != 0)
		{
			Transaction.Rollback(TEXT("Test.EntryReplaceClassPreflightRedCleanup"));
		}
		return LocalPassed;
	};

	bool Passed = RunPreflightFailure(TEXT("replace-class rejects missing name"),
		TEXT("Missing"), MakeEntryReplaceClassV2IntEntry(TEXT("Missing")),
		TEXT("AIProvider.Blackboard.KeyMissing"));
	Passed &= RunPreflightFailure(TEXT("replace-class rejects inherited-only name"),
		TEXT("Inherited"), MakeEntryReplaceClassV2IntEntry(TEXT("Inherited")),
		TEXT("AIProvider.Blackboard.KeyInherited"));
	Passed &= RunPreflightFailure(TEXT("replace-class rejects same class"),
		TEXT("Existing"), MakeEntryReplaceClassV2OriginalEntry(TEXT("Existing")),
		TEXT("AIProvider.Blackboard.KeyClassReplacementUnavailable"));
	Passed &= RunPreflightFailure(TEXT("replace-class rejects mismatched entry Name"),
		TEXT("Existing"), MakeEntryReplaceClassV2IntEntry(TEXT("Different")),
		TEXT("AIProvider.Blackboard.EntrySchemaInvalid"));
	Passed &= RunPreflightFailure(TEXT("replace-class rejects non-KeyType class"),
		TEXT("Existing"), MakeEntryReplaceClassV2IntEntry(TEXT("Existing")),
		TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments.Elements[1].Elements[2].Elements[1] =
				StringValue(UObject::StaticClass()->GetPathName());
		});
	const FAssetDocumentRuntimeValueDto StructInvalidDtoEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("")), StringValue(TEXT("")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({}, {}), StringValue(TEXT("Existing")), BooleanValue(false)});
	Passed &= RunPreflightFailure(TEXT("replace-class Struct invalid DTO is schema invalid"),
		TEXT("Existing"), StructInvalidDtoEntry, TEXT("AIProvider.Blackboard.EntrySchemaInvalid"));

	auto RunMaterializedFailure = [&](const TCHAR* Label,
		const EAssetDocumentAIBlackboardTestFailurePoint FailurePoint,
		const TCHAR* ExpectedCode,
		FAssetDocumentRuntimeValueDto Entry = MakeEntryReplaceClassV2IntEntry(TEXT("Existing")))
	{
		Blackboard->GetOutermost()->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		UBlackboardKeyType* const OldKeyType = Blackboard->Keys[0].KeyType;
		CollectGarbage(RF_NoFlags);
		const int32 DirectKeyTypesBefore = CountDirectBlackboardKeyTypes(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		SetAssetDocumentAIBlackboardTestFailurePoint(FailurePoint);
		const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryReplaceClassV2Invocation(
				Handle.GetValue(), *Blackboard, TEXT("Existing"), MoveTemp(Entry)),
			Handles, &Transaction);
		SetAssetDocumentAIBlackboardTestFailurePoint(
			EAssetDocumentAIBlackboardTestFailurePoint::None);
		bool LocalPassed = TestEqual(*(FString(Label) + TEXT(" fails")), Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed)
			&& TestTrue(*(FString(Label) + TEXT(" uses exact closed terminal")),
				HasDiagnostic(Outcome, ExpectedCode))
			&& TestTrue(*(FString(Label) + TEXT(" clears output/handles/ChangedSet")),
				Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty()
					&& Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty())
			&& TestEqual(*(FString(Label) + TEXT(" captures then enlists exactly once")),
				Transaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")});
		const FAssetDocumentContractValidationResult RollbackResult = Transaction.Rollback(
			FString::Printf(TEXT("Test.EntryReplaceClassFailure.%d"),
				static_cast<int32>(FailurePoint)));
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : RollbackResult.Diagnostics)
		{
			AddError(FString::Printf(TEXT("%s rollback diagnostic %s: %s"),
				Label, *Diagnostic.Code, *Diagnostic.Message));
		}
		LocalPassed &= TestTrue(*(FString(Label) + TEXT(" rolls back through Atomic")),
			RollbackResult.bValid);
		LocalPassed &= RequireObservation(*this,
			FString(Label) + TEXT(" restores exact preimage"), Before, *Blackboard);
		LocalPassed &= RequireObservation(*this,
			FString(Label) + TEXT(" restores child derived state"), ChildBefore, *Child);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" restores old exact KeyType instance")),
			Blackboard->Keys[0].KeyType.Get(), OldKeyType);
		CollectGarbage(RF_NoFlags);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" releases staged KeyType")),
			CountDirectBlackboardKeyTypes(*Blackboard), DirectKeyTypesBefore);
		return LocalPassed;
	};

	Passed &= RunMaterializedFailure(TEXT("replace-class staging failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassBeforeStaging,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("replace-class post-staging failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassAfterStaging,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("replace-class configuration failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassAfterConfiguration,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("replace-class pre-publication failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassBeforePublication,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("replace-class publication rejection"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassPublicationRejected,
		TEXT("AIProvider.AuthorityUnavailable"));
	Passed &= RunMaterializedFailure(TEXT("replace-class post-publication failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassAfterPublication,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunMaterializedFailure(TEXT("replace-class derived refresh failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassDerivedRefresh,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunMaterializedFailure(TEXT("replace-class readback failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassBeforeReadback,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunMaterializedFailure(TEXT("replace-class ChangedSet mismatch"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassChangedSetMismatch,
		TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"));
	FAssetDocumentRuntimeValueDto InvalidProperties =
		MakeEntryReplaceClassV2IntEntry(TEXT("Existing"));
	InvalidProperties.Elements[3] = ObjectValue({}, {});
	Passed &= RunMaterializedFailure(TEXT("replace-class invalid complete properties"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassAfterConfiguration,
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), MoveTemp(InvalidProperties));

	// Same-class replacement is rejected here; UPDATE remains its independent row
	// and is the only allowed same-class metadata/property writer.
	FBlackboardLifecycleTransaction UpdateTransaction;
	FBlackboardLifecycleHandleResolver UpdateHandles(UpdateTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> UpdateHandle = ResolveAIMutableAsset(
		*this, *Blackboard, UpdateTransaction, UpdateHandles);
	if (!UpdateHandle.IsSet()) return false;
	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest Update;
	Update.BlackboardPath = Blackboard->GetPathName();
	Update.KeyName = TEXT("Existing");
	Update.ExpectedClassPath =
		UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass()->GetPathName();
	Update.CanonicalProperties = ObjectValue(
		{TEXT("Int32Value"), TEXT("StringValue")},
		{IntegerValue(17), StringValue(TEXT("UPDATE only"))});
	Update.Description = TEXT("same-class update");
	Update.Category = TEXT("Boundary");
	Update.bInstanceSynced = false;
	UBlackboardKeyType* const BeforeUpdateInstance = Blackboard->Keys[0].KeyType;
	const FAssetDocumentPrimitiveOutcome UpdateOutcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeUpdateExistingInvocation(UpdateHandle.GetValue(), Update),
		UpdateHandles, &UpdateTransaction);
	Passed &= RequireSuccess(*this, TEXT("same-class boundary remains owned by UPDATE"), UpdateOutcome);
	Passed &= TestEqual(TEXT("UPDATE never dispatches to replace-class instance replacement"),
		Blackboard->Keys[0].KeyType.Get(), BeforeUpdateInstance);
	const FAssetDocumentContractValidationResult UpdateRollback =
		UpdateTransaction.Rollback(TEXT("Test.EntryReplaceClassUpdateBoundary"));
	Passed &= TestTrue(TEXT("UPDATE boundary rollback succeeds"), UpdateRollback.bValid);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2RemoveSuccessTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Remove.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2RemoveSuccessTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// RetainedAdmittedStructCanonical (SC-REM-001).
	// The expected nested Struct DTO is literal test ownership, never generated by
	// the production canonical READ path under test.
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2RemoveStruct_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* HardObject = NewObject<UBlackboardData>(
		Package, TEXT("HardObject"), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("SC-REM-001 Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("SC-REM-001 child fixture exists"), Child)
		|| !TestNotNull(TEXT("SC-REM-001 hard reference fixture exists"), HardObject))
	{
		return false;
	}
	TStrongObjectPtr<UBlackboardData> BlackboardGuard(Blackboard);
	TStrongObjectPtr<UBlackboardData> ChildGuard(Child);
	TStrongObjectPtr<UBlackboardData> HardObjectGuard(HardObject);
	AddExistingKey(*Blackboard, TEXT("First"), 1);
	AddExistingStockIntKey(*Blackboard, TEXT("RemoveMe"), 2);
	UBlackboardKeyType_Struct* StructKeyType = NewObject<UBlackboardKeyType_Struct>(
		Blackboard, TEXT("RetainedStructKeyType"), RF_Transactional);
	if (!TestNotNull(TEXT("SC-REM-001 retained Struct KeyType exists"), StructKeyType))
	{
		return false;
	}
	FProperty* DefaultValueProperty = FindFProperty<FProperty>(
		UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	if (!TestNotNull(TEXT("SC-REM-001 Struct DefaultValue property exists"),
		DefaultValueProperty))
	{
		return false;
	}
	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	auto& Stored = StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	Stored.ArrayValues = {9, -2};
	Stored.BoolValue = true;
	Stored.ByteValue = 255;
	Stored.ClassValue = UObject::StaticClass();
	Stored.DoubleValue = 12.5;
	Stored.DynamicValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Stored.DynamicValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Count = 4;
	Stored.DynamicValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Label = TEXT("dynamic");
	Stored.EnumValue = EAssetDocumentAIBlackboardCanonicalStructTestEnum::Second;
	Stored.FloatValue = 1.25f;
	Stored.HardObject = HardObject;
	Stored.Int64Value = MIN_int64 + 7;
	Stored.IntValue = -17;
	Stored.MapValues.Add(TEXT("b"), 2);
	Stored.MapValues.Add(TEXT("a"), 1);
	Stored.NameValue = FName(TEXT("CanonicalName_7"));
	Stored.NestedValue.Count = 3;
	Stored.NestedValue.Label = TEXT("nested");
	Stored.SetValues.Add(TEXT("zulu"));
	Stored.SetValues.Add(TEXT("alpha"));
	Stored.SoftClassValue = TSoftClassPtr<UObject>(UObject::StaticClass());
	Stored.SoftObjectValue = TSoftObjectPtr<UObject>(HardObject);
	Stored.StaticValues[0] = 5;
	Stored.StaticValues[1] = 6;
	Stored.StaticValues[2] = 7;
	Stored.StringValue = TEXT("canonical");
	Stored.TextValue = FText::AsCultureInvariant(TEXT("literal-equivalent"));
	Stored.UInt16Value = MAX_uint16;
	Stored.UInt32Value = MAX_uint32;
	Stored.UInt64Value = MAX_uint64;
	FPropertyChangedEvent DefaultValueChanged(
		DefaultValueProperty, EPropertyChangeType::ValueSet);
	DefaultValueChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(DefaultValueChanged);
	FBlackboardEntry StructEntry;
	StructEntry.EntryName = TEXT("RetainedStruct");
	StructEntry.EntryDescription = TEXT("Retained canonical Struct");
	StructEntry.EntryCategory = TEXT("Retention");
	StructEntry.KeyType = StructKeyType;
	StructEntry.bInstanceSynced = true;
	Blackboard->Keys.Add(MoveTemp(StructEntry));
	AddExistingKey(*Blackboard, TEXT("Last"), 3);
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Package->SetDirtyFlag(false);

	const auto StructValue = [](const UScriptStruct& Struct, TArray<FString> Keys,
		TArray<FAssetDocumentRuntimeValueDto> Values)
	{
		return ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue(MoveTemp(Keys), MoveTemp(Values)), StringValue(Struct.GetPathName())});
	};
	const FAssetDocumentRuntimeValueDto Nested = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(3), StringValue(TEXT("nested"))});
	const FAssetDocumentRuntimeValueDto Dynamic = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(4), StringValue(TEXT("dynamic"))});
	const FAssetDocumentRuntimeValueDto CanonicalStruct = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct(),
		{TEXT("ArrayValues"), TEXT("BoolValue"), TEXT("ByteValue"), TEXT("ClassValue"),
			TEXT("DoubleValue"), TEXT("DynamicValue"), TEXT("EnumValue"), TEXT("FloatValue"),
			TEXT("HardObject"), TEXT("Int64Value"), TEXT("IntValue"), TEXT("MapValues"),
			TEXT("NameValue"), TEXT("NestedValue"), TEXT("SetValues"), TEXT("SoftClassValue"),
			TEXT("SoftObjectValue"), TEXT("StaticValues"), TEXT("StringValue"), TEXT("TextValue"),
			TEXT("UInt16Value"), TEXT("UInt32Value"), TEXT("UInt64Value")},
		{ArrayValue({IntegerValue(9), IntegerValue(-2)}), BooleanValue(true), IntegerValue(255),
			ReferenceValue(TEXT("ClassRef"), *UObject::StaticClass()), NumberValue(12.5), Dynamic,
			ObjectValue({TEXT("Enum"), TEXT("Enumerator")},
				{StringValue(StaticEnum<EAssetDocumentAIBlackboardCanonicalStructTestEnum>()->GetPathName()),
					StringValue(TEXT("Second"))}),
			NumberValue(1.25), ReferenceValue(TEXT("AssetRef"), *HardObject),
			IntegerValue(MIN_int64 + 7), IntegerValue(-17),
			ArrayValue({ObjectValue({TEXT("Key"), TEXT("Value")},
				{StringValue(TEXT("a")), IntegerValue(1)}),
				ObjectValue({TEXT("Key"), TEXT("Value")},
					{StringValue(TEXT("b")), IntegerValue(2)})}),
			StringValue(TEXT("CanonicalName_7")), Nested,
			ArrayValue({StringValue(TEXT("zulu")), StringValue(TEXT("alpha"))}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftClassRef")), StringValue(UObject::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftAssetRef")), StringValue(HardObject->GetPathName())}),
			ArrayValue({IntegerValue(5), IntegerValue(6), IntegerValue(7)}),
			StringValue(TEXT("canonical")), StringValue(TEXT("literal-equivalent")),
			IntegerValue(MAX_uint16), IntegerValue(MAX_uint32),
			ObjectValue({TEXT("Kind"), TEXT("Value")},
				{StringValue(TEXT("UInt64")), StringValue(TEXT("18446744073709551615"))})});
	const FAssetDocumentRuntimeValueDto ExpectedStructEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("Retention")), StringValue(TEXT("Retained canonical Struct")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {CanonicalStruct}),
			StringValue(TEXT("RetainedStruct")), BooleanValue(true)});
	const FAssetDocumentRuntimeValueDto ExpectedOutput = ObjectValue(
		{TEXT("entries"), TEXT("ordered_local_key_names"), TEXT("removed_name")},
		{ArrayValue({MakeEntryReplaceClassV2OriginalEntry(TEXT("First"), 1),
			ExpectedStructEntry, MakeEntryReplaceClassV2OriginalEntry(TEXT("Last"), 3)}),
			ArrayValue({StringValue(TEXT("First")), StringValue(TEXT("RetainedStruct")),
				StringValue(TEXT("Last"))}), StringValue(TEXT("RemoveMe"))});
	TUniquePtr<FRemoveLoadedConsumerFixture> Consumer =
		MakeRemoveLoadedConsumerFixture(
			*this, *Blackboard, TEXT("Last"), TEXT("RemoveMe"));
	if (!Consumer.IsValid()) return false;
	const FBlackboard::FKey BeforeRetainedConsumerId =
		Blackboard->GetKeyID(TEXT("Last"));
	const FBlackboard::FKey BeforeRemovedConsumerId =
		Blackboard->GetKeyID(TEXT("RemoveMe"));
	if (!PrimeRemoveLoadedConsumer(
		*this, *Consumer, *Blackboard, TEXT("Last"), TEXT("RemoveMe"), 211))
	{
		return false;
	}
	const bool bBeforeConsumerTreeDirty = Consumer->Tree->GetOutermost()->IsDirty();
	const FBlackboardObservation Before(*Blackboard);
	const FBlackboardObservation ChildBefore(*Child);
	const auto FindLocalKeyType = [](const UBlackboardData& Owner, const FName Name)
	{
		const FBlackboardEntry* Entry = Owner.Keys.FindByPredicate(
			[Name](const FBlackboardEntry& Candidate)
			{
				return Candidate.EntryName == Name;
			});
		return Entry != nullptr ? Entry->KeyType.Get() : nullptr;
	};
	UBlackboardKeyType* const FirstKeyType = FindLocalKeyType(*Blackboard, TEXT("First"));
	UBlackboardKeyType* const RemovedKeyType = FindLocalKeyType(*Blackboard, TEXT("RemoveMe"));
	UBlackboardKeyType* const RetainedStructKeyType =
		FindLocalKeyType(*Blackboard, TEXT("RetainedStruct"));
	UBlackboardKeyType* const LastKeyType = FindLocalKeyType(*Blackboard, TEXT("Last"));
	if (!TestNotNull(TEXT("SC-REM-001 First KeyType exists"), FirstKeyType)
		|| !TestNotNull(TEXT("SC-REM-001 removed KeyType exists"), RemovedKeyType)
		|| !TestNotNull(TEXT("SC-REM-001 retained Struct KeyType is indexed"),
			RetainedStructKeyType)
		|| !TestNotNull(TEXT("SC-REM-001 Last KeyType exists"), LastKeyType))
	{
		return false;
	}
	FBlackboardLifecycleTransaction Transaction;
	FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
		*this, *Blackboard, Transaction, Handles);
	if (!Handle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryRemoveV2Invocation(Handle.GetValue(), *Blackboard, TEXT("RemoveMe")),
		Handles, &Transaction);
	bool Passed = RequireSuccess(*this, TEXT("SC-REM-001 retained admitted Struct remove"), Outcome);
	if (Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		Passed &= TestTrue(TEXT("SC-REM-001 returns exact complete retained DTO/order"),
			RuntimeValueEquals(Outcome.Value, ExpectedOutput));
		Passed &= TestEqual(TEXT("SC-REM-001 retains exact First KeyType"),
			FindLocalKeyType(*Blackboard, TEXT("First")), FirstKeyType);
		Passed &= TestEqual(TEXT("SC-REM-001 retains exact Struct KeyType"),
			FindLocalKeyType(*Blackboard, TEXT("RetainedStruct")), RetainedStructKeyType);
		Passed &= TestEqual(TEXT("SC-REM-001 retains exact Last KeyType"),
			FindLocalKeyType(*Blackboard, TEXT("Last")), LastKeyType);
		Passed &= TestEqual(TEXT("SC-REM-001 removes exact Name from target and child"),
			Blackboard->GetKeyID(TEXT("RemoveMe")), FBlackboard::InvalidKey);
		Passed &= TestEqual(TEXT("SC-REM-001 child removes exact inherited Name"),
			Child->GetKeyID(TEXT("RemoveMe")), FBlackboard::InvalidKey);
		Passed &= RequireRemoveLoadedConsumerForward(*this, *Consumer, *Blackboard,
			TEXT("Last"), TEXT("RemoveMe"), BeforeRetainedConsumerId,
			bBeforeConsumerTreeDirty);
		Passed &= TestEqual(TEXT("SC-REM-001 captures before one footprint"),
			Transaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")});
		Passed &= TestEqual(TEXT("SC-REM-001 enlists exactly one footprint"),
			Transaction.EnlistedFootprint.Num(), 1);
		Passed &= TestEqual(TEXT("SC-REM-001 reports named entry and order only"),
			Outcome.ChangedSet.Num(), 2);
		if (Outcome.ChangedSet.Num() == 2)
		{
			Passed &= TestEqual(TEXT("SC-REM-001 reports named-entry path first"),
				Outcome.ChangedSet[0].Path,
				FString::Printf(TEXT("asset:%s/Keys[name=RemoveMe]"), *Blackboard->GetPathName()));
			Passed &= TestEqual(TEXT("SC-REM-001 reports order path second"),
				Outcome.ChangedSet[1].Path,
				FString::Printf(TEXT("asset:%s/Keys.@Order"), *Blackboard->GetPathName()));
			for (const FAssetDocumentChangedRecord& Change : Outcome.ChangedSet)
			{
				Passed &= TestEqual(TEXT("SC-REM-001 uses exact ChangedSet authority"),
					Change.AuthorityId, FString(EntryRemoveV2Id));
				Passed &= TestEqual(TEXT("SC-REM-001 uses exact ChangedSet stable object"),
					Change.StableObjectId, Blackboard->GetPathName());
				Passed &= TestEqual(TEXT("SC-REM-001 uses Update ChangedSet kind"),
					Change.ChangeKind, FString(TEXT("Update")));
			}
		}
	}
	const FAssetDocumentContractValidationResult Rollback =
		Transaction.Rollback(TEXT("Test.SC-REM-001.Rollback"));
	Passed &= TestTrue(TEXT("SC-REM-001 late rollback succeeds"), Rollback.bValid);
	Passed &= RequireObservation(*this, TEXT("SC-REM-001 restores exact target"), Before, *Blackboard);
	Passed &= RequireObservation(*this, TEXT("SC-REM-001 restores exact child"), ChildBefore, *Child);
	Passed &= TestEqual(TEXT("SC-REM-001 restores exact removed KeyType instance"),
		FindLocalKeyType(*Blackboard, TEXT("RemoveMe")), RemovedKeyType);
	Passed &= RequireRemoveLoadedConsumerRollback(*this, *Consumer, *Blackboard,
		TEXT("Last"), TEXT("RemoveMe"), BeforeRetainedConsumerId,
		BeforeRemovedConsumerId, bBeforeConsumerTreeDirty, 311);

	// Boundary value characterization: only authored owned-local keys are projected;
	// engine-derived SelfActor remains at the physical local tail, outside that projection.
	const auto RunOwnedLocalBoundaryRemoval = [&](const TCHAR* Label,
		const TArray<FName>& BeforeNames, const FName RemovedName,
		const TArray<FName>& ExpectedNames)
	{
		UPackage* BoundaryPackage = CreatePackage(*FString::Printf(
			TEXT("/Temp/AssetDocumentAI/BlackboardV2RemoveBoundary_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		UBlackboardData* BoundaryBlackboard = NewObject<UBlackboardData>(
			BoundaryPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
		UBlackboardData* BoundaryChild = NewObject<UBlackboardData>(
			BoundaryPackage, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
		if (!TestNotNull(*(FString(Label) + TEXT(" Blackboard fixture exists")), BoundaryBlackboard)
			|| !TestNotNull(*(FString(Label) + TEXT(" child fixture exists")), BoundaryChild))
		{
			return false;
		}
		TStrongObjectPtr<UBlackboardData> BoundaryBlackboardGuard(BoundaryBlackboard);
		TStrongObjectPtr<UBlackboardData> BoundaryChildGuard(BoundaryChild);
		TArray<FBlackboardEntry> BoundaryDerivedKeys = MoveTemp(BoundaryBlackboard->Keys);
		BoundaryBlackboard->Keys.Reset();
		for (int32 Index = 0; Index < BeforeNames.Num(); ++Index)
		{
			AddExistingKey(*BoundaryBlackboard, BeforeNames[Index], 101 + Index);
		}
		BoundaryBlackboard->Keys.Append(MoveTemp(BoundaryDerivedKeys));
		BoundaryBlackboard->UpdateParentKeys();
		BoundaryBlackboard->UpdateKeyIDs();
		BoundaryBlackboard->UpdateIfHasSynchronizedKeys();
		BoundaryChild->Parent = BoundaryBlackboard;
		BoundaryChild->UpdateParentKeys();
		BoundaryChild->UpdateKeyIDs();
		BoundaryChild->UpdateIfHasSynchronizedKeys();
		BoundaryBlackboard->PropagateKeyChangesToDerivedBlackboardAssets();
		BoundaryPackage->SetDirtyFlag(false);

		const FBlackboardObservation BoundaryBefore(*BoundaryBlackboard);
		const FBlackboardObservation BoundaryChildBefore(*BoundaryChild);
		TMap<FName, UBlackboardKeyType*> BeforeKeyTypes;
		for (const FBlackboardEntry& Entry : BoundaryBlackboard->Keys)
		{
			BeforeKeyTypes.Add(Entry.EntryName, Entry.KeyType.Get());
		}
		FBlackboardLifecycleTransaction BoundaryTransaction;
		FBlackboardLifecycleHandleResolver BoundaryHandles(BoundaryTransaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> BoundaryHandle = ResolveAIMutableAsset(
			*this, *BoundaryBlackboard, BoundaryTransaction, BoundaryHandles);
		if (!BoundaryHandle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome BoundaryOutcome =
			CreateAssetDocumentAIPrimitiveProvider()->Invoke(
				MakeEntryRemoveV2Invocation(
					BoundaryHandle.GetValue(), *BoundaryBlackboard, RemovedName.ToString()),
				BoundaryHandles, &BoundaryTransaction);
		bool BoundaryPassed = RequireSuccess(*this, FString(Label) + TEXT(" owned-local removal"),
			BoundaryOutcome);
		if (BoundaryOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		{
			TArray<FAssetDocumentRuntimeValueDto> ExpectedEntries;
			TArray<FAssetDocumentRuntimeValueDto> ExpectedOrder;
			for (const FName Name : ExpectedNames)
			{
				const int32 OriginalIndex = BeforeNames.IndexOfByKey(Name);
				ExpectedEntries.Add(MakeEntryReplaceClassV2OriginalEntry(
					Name.ToString(), 101 + OriginalIndex));
				ExpectedOrder.Add(StringValue(Name.ToString()));
			}
			const FAssetDocumentRuntimeValueDto ExpectedBoundaryOutput = ObjectValue(
				{TEXT("entries"), TEXT("ordered_local_key_names"), TEXT("removed_name")},
				{ArrayValue(MoveTemp(ExpectedEntries)), ArrayValue(MoveTemp(ExpectedOrder)),
					StringValue(RemovedName.ToString())});
			BoundaryPassed &= TestTrue(*(FString(Label) + TEXT(" returns exact owned-local projection")),
				RuntimeValueEquals(BoundaryOutcome.Value, ExpectedBoundaryOutput));
			TArray<FName> ActualNames;
			for (const FBlackboardEntry& Entry : BoundaryBlackboard->Keys)
			{
				ActualNames.Add(Entry.EntryName);
			}
			TArray<FName> ExpectedStoredNames = ExpectedNames;
			ExpectedStoredNames.Add(FBlackboard::KeySelf);
			BoundaryPassed &= TestEqual(*(FString(Label) + TEXT(" leaves full stored local order")),
				ActualNames, ExpectedStoredNames);
			BoundaryPassed &= TestTrue(*(FString(Label) + TEXT(" keeps SelfActor engine-derived")),
				BoundaryBlackboard->Keys.Num() == ExpectedStoredNames.Num()
					&& IsEngineDerivedSelfKey(BoundaryBlackboard->Keys.Last()));
			BoundaryPassed &= TestTrue(*(FString(Label) + TEXT(" preserves engine-derived SelfActor")),
				BoundaryBlackboard->GetKeyID(FBlackboard::KeySelf) != FBlackboard::InvalidKey);
			BoundaryPassed &= TestEqual(*(FString(Label) + TEXT(" removes exact local name")),
				BoundaryBlackboard->GetKeyID(RemovedName), FBlackboard::InvalidKey);
			BoundaryPassed &= TestEqual(*(FString(Label) + TEXT(" removes exact child name")),
				BoundaryChild->GetKeyID(RemovedName), FBlackboard::InvalidKey);
			for (const FName Name : ExpectedNames)
			{
				BoundaryPassed &= TestEqual(*(FString(Label) + TEXT(" retains exact KeyType identity ")
					+ Name.ToString()), FindLocalKeyType(*BoundaryBlackboard, Name),
					BeforeKeyTypes.FindRef(Name));
			}
			BoundaryPassed &= TestEqual(*(FString(Label) + TEXT(" captures before one footprint")),
				BoundaryTransaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")});
			BoundaryPassed &= TestEqual(*(FString(Label) + TEXT(" enlists exactly one footprint")),
				BoundaryTransaction.EnlistedFootprint.Num(), 1);
		}
		const FAssetDocumentContractValidationResult BoundaryRollback = BoundaryTransaction.Rollback(
			FString(Label) + TEXT(".Rollback"));
		BoundaryPassed &= TestTrue(*(FString(Label) + TEXT(" rollback succeeds")),
			BoundaryRollback.bValid);
		BoundaryPassed &= RequireObservation(*this, FString(Label) + TEXT(" restores exact target"),
			BoundaryBefore, *BoundaryBlackboard);
		BoundaryPassed &= RequireObservation(*this, FString(Label) + TEXT(" restores exact child"),
			BoundaryChildBefore, *BoundaryChild);
		for (const FName Name : BeforeNames)
		{
			BoundaryPassed &= TestEqual(*(FString(Label) + TEXT(" restores exact KeyType identity ")
				+ Name.ToString()), FindLocalKeyType(*BoundaryBlackboard, Name),
				BeforeKeyTypes.FindRef(Name));
		}
		return BoundaryPassed;
	};
	Passed &= RunOwnedLocalBoundaryRemoval(TEXT("SC-REM-001 first-index"),
		{TEXT("RemoveFirst"), TEXT("RetainedMiddle"), TEXT("RetainedLast")}, TEXT("RemoveFirst"),
		{TEXT("RetainedMiddle"), TEXT("RetainedLast")});
	Passed &= RunOwnedLocalBoundaryRemoval(TEXT("SC-REM-001 last-index"),
		{TEXT("RetainedFirst"), TEXT("RetainedMiddle"), TEXT("RemoveLast")}, TEXT("RemoveLast"),
		{TEXT("RetainedFirst"), TEXT("RetainedMiddle")});
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2RemoveFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Remove.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2RemoveFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	using ELoadedFailure = AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ELoadedConsumerCompletionTestFailurePoint;
	// RetainedStructClosedAndLateRollback (SC-REM-002).
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2RemoveFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Parent = NewObject<UBlackboardData>(
		Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("SC-REM-002 parent exists"), Parent)
		|| !TestNotNull(TEXT("SC-REM-002 Blackboard exists"), Blackboard)
		|| !TestNotNull(TEXT("SC-REM-002 child exists"), Child)) return false;
	TStrongObjectPtr<UBlackboardData> ParentGuard(Parent);
	TStrongObjectPtr<UBlackboardData> BlackboardGuard(Blackboard);
	TStrongObjectPtr<UBlackboardData> ChildGuard(Child);
	AddExistingKey(*Parent, TEXT("InheritedOnly"), 11);
	AddExistingStockIntKey(*Blackboard, TEXT("RemoveMe"), 21);
	UBlackboardKeyType_Struct* StructKeyType = NewObject<UBlackboardKeyType_Struct>(
		Blackboard, TEXT("RetainedStructKeyType"), RF_Transactional);
	FProperty* DefaultValueProperty = FindFProperty<FProperty>(
		UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	if (!TestNotNull(TEXT("SC-REM-002 Struct KeyType exists"), StructKeyType)
		|| !TestNotNull(TEXT("SC-REM-002 DefaultValue property exists"),
			DefaultValueProperty)) return false;
	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructNested>();
	auto& InitialPayload = StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>();
	InitialPayload.Count = 7;
	InitialPayload.Label = TEXT("retained");
	FPropertyChangedEvent InitialChanged(
		DefaultValueProperty, EPropertyChangeType::ValueSet);
	InitialChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(InitialChanged);
	FBlackboardEntry StructEntry;
	StructEntry.EntryName = TEXT("RetainedStruct");
	StructEntry.KeyType = StructKeyType;
	Blackboard->Keys.Add(MoveTemp(StructEntry));
	AddExistingKey(*Blackboard, TEXT("Last"), 31);
	Parent->UpdateParentKeys();
	Parent->UpdateKeyIDs();
	Parent->UpdateIfHasSynchronizedKeys();
	Blackboard->Parent = Parent;
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Parent->PropagateKeyChangesToDerivedBlackboardAssets();
	Package->SetDirtyFlag(false);

	const auto FindLocalKeyType = [](const UBlackboardData& Owner, const FName Name)
	{
		const FBlackboardEntry* Entry = Owner.Keys.FindByPredicate(
			[Name](const FBlackboardEntry& Candidate)
			{
				return Candidate.EntryName == Name;
			});
		return Entry != nullptr ? Entry->KeyType.Get() : nullptr;
	};
	auto RunPreflightFailure = [&](const TCHAR* Label, const TCHAR* Name,
		const TCHAR* ExpectedCode)
	{
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		UBlackboardKeyType* const Removed = FindLocalKeyType(*Blackboard, TEXT("RemoveMe"));
		UBlackboardKeyType* const Retained =
			FindLocalKeyType(*Blackboard, TEXT("RetainedStruct"));
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome =
			CreateAssetDocumentAIPrimitiveProvider()->Invoke(
				MakeEntryRemoveV2Invocation(Handle.GetValue(), *Blackboard, Name),
				Handles, &Transaction);
		return RequireExactFailureTerminal(
				*this, Label, Outcome, Before, *Blackboard, Transaction, ExpectedCode)
			&& RequireObservation(*this, FString(Label) + TEXT(" preserves child"),
				ChildBefore, *Child)
			&& TestEqual(*(FString(Label) + TEXT(" preserves removed KeyType")),
				FindLocalKeyType(*Blackboard, TEXT("RemoveMe")), Removed)
			&& TestEqual(*(FString(Label) + TEXT(" preserves retained Struct KeyType")),
				FindLocalKeyType(*Blackboard, TEXT("RetainedStruct")), Retained);
	};

	bool Passed = RunPreflightFailure(TEXT("SC-REM-002 rejects missing local Name"),
		TEXT("Missing"), TEXT("AIProvider.Blackboard.KeyMissing"));
	Passed &= RunPreflightFailure(TEXT("SC-REM-002 rejects inherited Name"),
		TEXT("InheritedOnly"), TEXT("AIProvider.Blackboard.KeyInherited"));
	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructUnsupported>();
	FPropertyChangedEvent UnsupportedChanged(
		DefaultValueProperty, EPropertyChangeType::ValueSet);
	UnsupportedChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(UnsupportedChanged);
	Package->SetDirtyFlag(false);
	Passed &= RunPreflightFailure(TEXT("SC-REM-002 closes unsupported retained Struct"),
		TEXT("RemoveMe"), TEXT("AIProvider.AuthorityUnavailable"));

	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructNested>();
	auto& AdmittedPayload = StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>();
	AdmittedPayload.Count = 17;
	AdmittedPayload.Label = TEXT("late-rollback");
	FPropertyChangedEvent AdmittedChanged(
		DefaultValueProperty, EPropertyChangeType::ValueSet);
	AdmittedChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(AdmittedChanged);
	Package->SetDirtyFlag(false);
	TUniquePtr<FRemoveLoadedConsumerFixture> Consumer =
		MakeRemoveLoadedConsumerFixture(
			*this, *Blackboard, TEXT("Last"), TEXT("RemoveMe"));
	if (!Consumer.IsValid()) return false;
	int32 ConsumerRuntimeValue = 701;

	auto RunLateFailure = [&](const TCHAR* Label,
		const EAssetDocumentAIBlackboardTestFailurePoint RowFailure,
		const ELoadedFailure LoadedFailure, const TCHAR* ExpectedCode)
	{
		const FBlackboard::FKey BeforeRetainedConsumerId =
			Blackboard->GetKeyID(TEXT("Last"));
		const FBlackboard::FKey BeforeRemovedConsumerId =
			Blackboard->GetKeyID(TEXT("RemoveMe"));
		const int32 PrimedRuntimeValue = ConsumerRuntimeValue++;
		if (!PrimeRemoveLoadedConsumer(*this, *Consumer, *Blackboard,
			TEXT("Last"), TEXT("RemoveMe"), PrimedRuntimeValue))
		{
			return false;
		}
		const bool bBeforeConsumerTreeDirty =
			Consumer->Tree->GetOutermost()->IsDirty();
		const FBlackboardObservation Before(*Blackboard);
		const FBlackboardObservation ChildBefore(*Child);
		UBlackboardKeyType* const Removed = FindLocalKeyType(*Blackboard, TEXT("RemoveMe"));
		UBlackboardKeyType* const Retained =
			FindLocalKeyType(*Blackboard, TEXT("RetainedStruct"));
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint RowPoint(RowFailure);
			FScopedLoadedConsumerCompletionTestFailurePoint LoadedPoint(LoadedFailure);
			Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
				MakeEntryRemoveV2Invocation(
					Handle.GetValue(), *Blackboard, TEXT("RemoveMe")),
				Handles, &Transaction);
		}
		bool LocalPassed = TestEqual(*(FString(Label) + TEXT(" fails")),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed)
			&& TestEqual(*(FString(Label) + TEXT(" has one diagnostic")),
				Outcome.Diagnostics.Num(), 1)
			&& TestTrue(*(FString(Label) + TEXT(" clears output/handles/ChangedSet")),
				Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty()
					&& Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty())
			&& TestEqual(*(FString(Label) + TEXT(" captures then enlists exactly once")),
				Transaction.Order, TArray<FString>{TEXT("action"), TEXT("footprint")});
		if (Outcome.Diagnostics.Num() == 1)
		{
			LocalPassed &= TestEqual(*(FString(Label) + TEXT(" uses frozen terminal")),
				Outcome.Diagnostics[0].Code, FString(ExpectedCode));
		}
		const FAssetDocumentContractValidationResult Rollback = Transaction.Rollback(
			FString::Printf(TEXT("Test.SC-REM-002.%d"), static_cast<int32>(RowFailure)));
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Rollback.Diagnostics)
		{
			AddError(FString::Printf(TEXT("%s rollback diagnostic %s: %s"),
				Label, *Diagnostic.Code, *Diagnostic.Message));
		}
		LocalPassed &= TestTrue(*(FString(Label) + TEXT(" rollback succeeds")),
			Rollback.bValid);
		LocalPassed &= RequireObservation(*this,
			FString(Label) + TEXT(" restores exact target"), Before, *Blackboard);
		LocalPassed &= RequireObservation(*this,
			FString(Label) + TEXT(" restores exact child"), ChildBefore, *Child);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" restores removed KeyType")),
			FindLocalKeyType(*Blackboard, TEXT("RemoveMe")), Removed);
		LocalPassed &= TestEqual(*(FString(Label) + TEXT(" restores retained Struct KeyType")),
			FindLocalKeyType(*Blackboard, TEXT("RetainedStruct")), Retained);
		LocalPassed &= RequireRemoveLoadedConsumerRollback(*this, *Consumer, *Blackboard,
			TEXT("Last"), TEXT("RemoveMe"), BeforeRetainedConsumerId,
			BeforeRemovedConsumerId, bBeforeConsumerTreeDirty,
			PrimedRuntimeValue + 1000);
		return LocalPassed;
	};

	// OrderedKeysLifecycleUnavailable is the deterministic publication-rejection
	// proof: output is zero-residue and the outer transaction restores the full preimage.
	Passed &= RunLateFailure(TEXT("SC-REM-002 publication rejection"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryRemovePublicationRejected,
		ELoadedFailure::None,
		TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"));
	Passed &= RunLateFailure(TEXT("SC-REM-002 loaded consumer before"),
		EAssetDocumentAIBlackboardTestFailurePoint::None,
		ELoadedFailure::BeforeLoadedConsumerForwardCompletion,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunLateFailure(TEXT("SC-REM-002 loaded consumer after"),
		EAssetDocumentAIBlackboardTestFailurePoint::None,
		ELoadedFailure::AfterLoadedConsumerForwardCompletion,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunLateFailure(TEXT("SC-REM-002 derived ID late failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::AfterLocalKeyIdRefresh,
		ELoadedFailure::None, TEXT("AIProvider.OperationRejected"));
	Passed &= RunLateFailure(TEXT("SC-REM-002 readback failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryRemoveBeforeReadback,
		ELoadedFailure::None, TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	Passed &= RunLateFailure(TEXT("SC-REM-002 ChangedSet mismatch"),
		EAssetDocumentAIBlackboardTestFailurePoint::EntryRemoveChangedSetMismatch,
		ELoadedFailure::None, TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2RemoveSaveReloadTest,
	"UEAssetDoc.AIProvider.BlackboardV2.Remove.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2RemoveSaveReloadTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// RetainedAdmittedStructCanonical (SC-REM-003).
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentAIBlackboardRemove/BlackboardRemoveStructReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString ChildPath = PackageName + TEXT(".Child");
	const FString HardObjectPath = PackageName + TEXT(".HardObject");
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PackageFileName, false, true);
	};

	UPackage* Package = CreatePackage(*PackageName);
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* HardObject = NewObject<UBlackboardData>(
		Package, TEXT("HardObject"), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("SC-REM-003 Blackboard exists"), Blackboard)
		|| !TestNotNull(TEXT("SC-REM-003 child exists"), Child)
		|| !TestNotNull(TEXT("SC-REM-003 hard reference exists"), HardObject)) return false;
	AddExistingKey(*Blackboard, TEXT("First"), 1);
	AddExistingKey(*Blackboard, TEXT("RemoveMe"), 2);
	UBlackboardKeyType_Struct* StructKeyType = NewObject<UBlackboardKeyType_Struct>(
		Blackboard, TEXT("RetainedStructKeyType"), RF_Transactional);
	FProperty* DefaultValueProperty = FindFProperty<FProperty>(
		UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	if (!TestNotNull(TEXT("SC-REM-003 Struct KeyType exists"), StructKeyType)
		|| !TestNotNull(TEXT("SC-REM-003 DefaultValue property exists"),
			DefaultValueProperty)) return false;
	StructKeyType->PreEditChange(DefaultValueProperty);
	StructKeyType->DefaultValue.InitializeAs<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	auto& Stored = StructKeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	Stored.ArrayValues = {9, -2};
	Stored.BoolValue = true;
	Stored.ByteValue = 255;
	Stored.ClassValue = UObject::StaticClass();
	Stored.DoubleValue = 12.5;
	Stored.DynamicValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Stored.DynamicValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>().Count = 4;
	Stored.DynamicValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructNested>().Label = TEXT("dynamic");
	Stored.EnumValue = EAssetDocumentAIBlackboardCanonicalStructTestEnum::Second;
	Stored.FloatValue = 1.25f;
	Stored.HardObject = HardObject;
	Stored.Int64Value = MIN_int64 + 7;
	Stored.IntValue = -17;
	Stored.MapValues.Add(TEXT("b"), 2);
	Stored.MapValues.Add(TEXT("a"), 1);
	Stored.NameValue = FName(TEXT("CanonicalName_7"));
	Stored.NestedValue.Count = 3;
	Stored.NestedValue.Label = TEXT("nested");
	Stored.SetValues.Add(TEXT("zulu"));
	Stored.SetValues.Add(TEXT("alpha"));
	Stored.SoftClassValue = TSoftClassPtr<UObject>(UObject::StaticClass());
	Stored.SoftObjectValue = TSoftObjectPtr<UObject>(HardObject);
	Stored.StaticValues[0] = 5;
	Stored.StaticValues[1] = 6;
	Stored.StaticValues[2] = 7;
	Stored.StringValue = TEXT("canonical");
	Stored.TextValue = FText::AsCultureInvariant(TEXT("literal-equivalent"));
	Stored.UInt16Value = MAX_uint16;
	Stored.UInt32Value = MAX_uint32;
	Stored.UInt64Value = MAX_uint64;
	FPropertyChangedEvent DefaultValueChanged(
		DefaultValueProperty, EPropertyChangeType::ValueSet);
	DefaultValueChanged.MemberProperty = DefaultValueProperty;
	StructKeyType->PostEditChangeProperty(DefaultValueChanged);
	FBlackboardEntry StructEntry;
	StructEntry.EntryName = TEXT("RetainedStruct");
	StructEntry.EntryDescription = TEXT("Retained canonical Struct");
	StructEntry.EntryCategory = TEXT("Retention");
	StructEntry.KeyType = StructKeyType;
	StructEntry.bInstanceSynced = true;
	Blackboard->Keys.Add(MoveTemp(StructEntry));
	AddExistingKey(*Blackboard, TEXT("Last"), 3);
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys();
	Child->UpdateKeyIDs();
	Child->UpdateIfHasSynchronizedKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Package->SetDirtyFlag(false);

	const auto StructValue = [](const UScriptStruct& Struct, TArray<FString> Keys,
		TArray<FAssetDocumentRuntimeValueDto> Values)
	{
		return ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue(MoveTemp(Keys), MoveTemp(Values)), StringValue(Struct.GetPathName())});
	};
	const FAssetDocumentRuntimeValueDto Nested = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(3), StringValue(TEXT("nested"))});
	const FAssetDocumentRuntimeValueDto Dynamic = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(4), StringValue(TEXT("dynamic"))});
	const FAssetDocumentRuntimeValueDto CanonicalStruct = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct(),
		{TEXT("ArrayValues"), TEXT("BoolValue"), TEXT("ByteValue"), TEXT("ClassValue"),
			TEXT("DoubleValue"), TEXT("DynamicValue"), TEXT("EnumValue"), TEXT("FloatValue"),
			TEXT("HardObject"), TEXT("Int64Value"), TEXT("IntValue"), TEXT("MapValues"),
			TEXT("NameValue"), TEXT("NestedValue"), TEXT("SetValues"), TEXT("SoftClassValue"),
			TEXT("SoftObjectValue"), TEXT("StaticValues"), TEXT("StringValue"), TEXT("TextValue"),
			TEXT("UInt16Value"), TEXT("UInt32Value"), TEXT("UInt64Value")},
		{ArrayValue({IntegerValue(9), IntegerValue(-2)}), BooleanValue(true), IntegerValue(255),
			ReferenceValue(TEXT("ClassRef"), *UObject::StaticClass()), NumberValue(12.5), Dynamic,
			ObjectValue({TEXT("Enum"), TEXT("Enumerator")},
				{StringValue(StaticEnum<EAssetDocumentAIBlackboardCanonicalStructTestEnum>()->GetPathName()),
					StringValue(TEXT("Second"))}),
			NumberValue(1.25), ReferenceValue(TEXT("AssetRef"), *HardObject),
			IntegerValue(MIN_int64 + 7), IntegerValue(-17),
			ArrayValue({ObjectValue({TEXT("Key"), TEXT("Value")},
				{StringValue(TEXT("a")), IntegerValue(1)}),
				ObjectValue({TEXT("Key"), TEXT("Value")},
					{StringValue(TEXT("b")), IntegerValue(2)})}),
			StringValue(TEXT("CanonicalName_7")), Nested,
			ArrayValue({StringValue(TEXT("zulu")), StringValue(TEXT("alpha"))}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftClassRef")), StringValue(UObject::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftAssetRef")), StringValue(HardObjectPath)}),
			ArrayValue({IntegerValue(5), IntegerValue(6), IntegerValue(7)}),
			StringValue(TEXT("canonical")), StringValue(TEXT("literal-equivalent")),
			IntegerValue(MAX_uint16), IntegerValue(MAX_uint32),
			ObjectValue({TEXT("Kind"), TEXT("Value")},
				{StringValue(TEXT("UInt64")), StringValue(TEXT("18446744073709551615"))})});
	const FAssetDocumentRuntimeValueDto ExpectedStructEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(TEXT("Retention")), StringValue(TEXT("Retained canonical Struct")),
			ReferenceValue(TEXT("ClassRef"), *UBlackboardKeyType_Struct::StaticClass()),
			ObjectValue({TEXT("DefaultValue")}, {CanonicalStruct}),
			StringValue(TEXT("RetainedStruct")), BooleanValue(true)});
	const FAssetDocumentRuntimeValueDto ExpectedOutput = ObjectValue(
		{TEXT("entries"), TEXT("ordered_local_key_names"), TEXT("removed_name")},
		{ArrayValue({MakeEntryReplaceClassV2OriginalEntry(TEXT("First"), 1),
			ExpectedStructEntry, MakeEntryReplaceClassV2OriginalEntry(TEXT("Last"), 3)}),
			ArrayValue({StringValue(TEXT("First")), StringValue(TEXT("RetainedStruct")),
				StringValue(TEXT("Last"))}), StringValue(TEXT("RemoveMe"))});

	bool Passed = true;
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome =
			CreateAssetDocumentAIPrimitiveProvider()->Invoke(
				MakeEntryRemoveV2Invocation(
					Handle.GetValue(), *Blackboard, TEXT("RemoveMe")),
				Handles, &Transaction);
		Passed &= RequireSuccess(*this, TEXT("SC-REM-003 remove before persistence"), Outcome);
		Passed &= TestTrue(TEXT("SC-REM-003 returns complete retained DTO/order"),
			RuntimeValueEquals(Outcome.Value, ExpectedOutput));
		Passed &= TestTrue(TEXT("SC-REM-003 returns no handles and exact ChangedSet"),
			Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.Num() == 2);
		Passed &= TestEqual(TEXT("SC-REM-003 removes target before persistence"),
			Blackboard->GetKeyID(TEXT("RemoveMe")), FBlackboard::InvalidKey);
		Passed &= TestEqual(TEXT("SC-REM-003 removes target from child before persistence"),
			Child->GetKeyID(TEXT("RemoveMe")), FBlackboard::InvalidKey);
		Passed &= TestTrue(TEXT("SC-REM-003 transaction becomes commit-ready"),
			Transaction.MarkReadyToCommit().bValid);
		Passed &= TestTrue(TEXT("SC-REM-003 transaction commits before persistence"),
			Transaction.Commit(MakeStructuralCompletionReceipt(Transaction)).bValid);
	}
	Passed &= TestFalse(TEXT("SC-REM-003 provider performs no persistence"),
		IFileManager::Get().FileExists(*PackageFileName));
	Passed &= TestTrue(TEXT("SC-REM-003 published package is dirty before external save"),
		Package->IsDirty());
	Package->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	Passed &= TestTrue(TEXT("SC-REM-003 external lifecycle saves the result"),
		UPackage::SavePackage(Package, Blackboard, *PackageFileName, SaveArgs));
	if (!Passed) return false;
	Passed &= TestTrue(TEXT("SC-REM-003 package unloads before fresh reload"),
		UPackageTools::UnloadPackages({Package}));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Passed &= TestNull(TEXT("SC-REM-003 package is absent before fresh load"),
		FindPackage(nullptr, *PackageName));
	UBlackboardData* Reloaded = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	UBlackboardData* ReloadedChild = LoadObject<UBlackboardData>(nullptr, *ChildPath);
	if (!TestNotNull(TEXT("SC-REM-003 Blackboard fresh-reloads"), Reloaded)
		|| !TestNotNull(TEXT("SC-REM-003 child fresh-reloads"), ReloadedChild)) return false;
	Passed &= TestEqual(TEXT("SC-REM-003 fresh target remains absent"),
		Reloaded->GetKeyID(TEXT("RemoveMe")), FBlackboard::InvalidKey);
	Passed &= TestEqual(TEXT("SC-REM-003 fresh child target remains absent"),
		ReloadedChild->GetKeyID(TEXT("RemoveMe")), FBlackboard::InvalidKey);
	const FBlackboardEntry* ReloadedStructEntry = Reloaded->Keys.FindByPredicate(
		[](const FBlackboardEntry& Entry)
		{
			return Entry.EntryName == TEXT("RetainedStruct");
		});
	if (!TestNotNull(TEXT("SC-REM-003 retained Struct fresh-reloads"),
		ReloadedStructEntry)) return false;
	const UBlackboardKeyType_Struct* ReloadedStruct =
		Cast<UBlackboardKeyType_Struct>(ReloadedStructEntry->KeyType);
	if (!TestNotNull(TEXT("SC-REM-003 exact Struct KeyType fresh-reloads"),
		ReloadedStruct)) return false;
	Passed &= TestTrue(TEXT("SC-REM-003 exact dynamic Struct identity survives"),
		ReloadedStruct->DefaultValue.GetScriptStruct()
			== FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct());
	const auto* ReloadedPayload = ReloadedStruct->DefaultValue.GetPtr<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	if (!TestNotNull(TEXT("SC-REM-003 admitted native payload survives"),
		ReloadedPayload)) return false;
	Passed &= TestTrue(TEXT("SC-REM-003 nested/container/reference values survive"),
		ReloadedPayload->NestedValue.Count == 3
			&& ReloadedPayload->DynamicValue.GetScriptStruct()
				== FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()
			&& ReloadedPayload->MapValues.FindRef(TEXT("a")) == 1
			&& ReloadedPayload->SetValues.Contains(TEXT("alpha"))
			&& GetPathNameSafe(ReloadedPayload->HardObject) == HardObjectPath
			&& ReloadedPayload->SoftObjectValue.ToSoftObjectPath().ToString() == HardObjectPath);
	const FString* ReloadedTextSource = FTextInspector::GetSourceString(
		ReloadedPayload->TextValue);
	Passed &= TestTrue(TEXT("SC-REM-003 literal-equivalent text survives"),
		ReloadedTextSource != nullptr && *ReloadedTextSource == TEXT("literal-equivalent"));

	const FBlackboardObservation BeforeReadback(*Reloaded);
	FBlackboardLifecycleTransaction ReadbackTransaction;
	FBlackboardLifecycleHandleResolver ReadbackHandles(
		ReadbackTransaction.GetMutationToken());
	const FAssetDocumentPrimitiveOutcome EntryReadback =
		CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeEntryReadbackInvocation(*Reloaded,
				ReadbackTransaction.GetMutationToken(), TEXT("RetainedStruct")),
			ReadbackHandles, nullptr);
	Passed &= RequireSuccess(*this, TEXT("SC-REM-003 fresh canonical entry readback"),
		EntryReadback);
	Passed &= TestTrue(TEXT("SC-REM-003 fresh Struct DTO is exact"),
		EntryReadback.Value.ObjectKeys == TArray<FString>{TEXT("entry")}
			&& EntryReadback.Value.Elements.Num() == 1
			&& RuntimeValueEquals(EntryReadback.Value.Elements[0], ExpectedStructEntry));
	const FAssetDocumentPrimitiveOutcome LocalReadback =
		CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeLocalKeysReadbackInvocation(
				*Reloaded, ReadbackTransaction.GetMutationToken()),
			ReadbackHandles, nullptr);
	Passed &= RequireCanonicalLocalKeysReadback(*this, LocalReadback,
		{FBlackboard::KeySelf.ToString(), TEXT("First"), TEXT("RetainedStruct"), TEXT("Last")});
	Passed &= RequireObservation(*this, TEXT("SC-REM-003 fresh READ leaves no residue"),
		BeforeReadback, *Reloaded);
	Passed &= TestTrue(TEXT("SC-REM-003 fresh READ has no action or footprint"),
		ReadbackTransaction.ActionEnlistCount == 0
			&& ReadbackTransaction.FootprintEnlistCount == 0
			&& ReadbackTransaction.EnlistedFootprint.IsEmpty());
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ParentSetTest,
	"UEAssetDoc.AIProvider.BlackboardV2.ParentSet.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ParentSetTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: a real Parent change uses the two ordered rollback
	//   boundaries, returns the exact one-member DTO/ChangedSet, and a same-parent
	//   request is a zero-action, zero-footprint, zero-dirty no-op.
	// - Candidate technique: state-transition testing across actual-change,
	//   rollback and identity no-op.
	// - Risk or gap: v1's mutable overload/richer output or an unnecessary write
	//   can superficially appear successful.
	// - Test: change ParentA to ParentB, roll back, then request ParentA.
	// - Expected observable outcome: exact Parent/derived closure and dirty-last
	//   rollback, followed by a no-op with empty ChangedSet.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ParentSet_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* ParentA = NewObject<UBlackboardData>(
		Package, TEXT("ParentA"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* ParentB = NewObject<UBlackboardData>(
		Package, TEXT("ParentB"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("Parent set target fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("Parent set ParentA fixture exists"), ParentA)
		|| !TestNotNull(TEXT("Parent set ParentB fixture exists"), ParentB)
		|| !TestNotNull(TEXT("Parent set child fixture exists"), Child)) return false;

	Blackboard->Keys.Reset();
	ParentA->Keys.Reset();
	ParentB->Keys.Reset();
	Child->Keys.Reset();
	AddExistingKey(*Blackboard, TEXT("TargetLocal"), 1);
	AddExistingKey(*ParentA, TEXT("FromParentA"), 2);
	AddExistingKey(*ParentB, TEXT("FromParentB"), 3);
	AddExistingKey(*Child, TEXT("ChildLocal"), 4);
	ParentA->UpdateParentKeys(); ParentA->UpdateKeyIDs();
	ParentB->UpdateParentKeys(); ParentB->UpdateKeyIDs();
	Blackboard->Parent = ParentA;
	Blackboard->UpdateParentKeys(); Blackboard->UpdateKeyIDs();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys(); Child->UpdateKeyIDs();
	Package->SetDirtyFlag(false);
	const FBlackboardObservation Before(*Blackboard);
	const FBlackboard::FKey ParentAKeyBefore = Child->GetKeyID(TEXT("FromParentA"));
	if (!TestTrue(TEXT("child fixture initially resolves ParentA projection"),
		Child->GetKey(ParentAKeyBefore) != nullptr)) return false;

	FBlackboardLifecycleTransaction Transaction;
	FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
		*this, *Blackboard, Transaction, Handles);
	if (!Handle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
		MakeParentSetV2Invocation(Handle.GetValue(), *Blackboard, ParentB),
		Handles, &Transaction);
	bool Passed = RequireSuccess(*this, TEXT("Parent set v2 actual change"), Outcome);
	Passed &= TestTrue(TEXT("Parent set v2 returns exact canonical Parent DTO"),
		RuntimeValueEquals(Outcome.Value,
			ObjectValue({TEXT("parent_path_or_null")},
				{StringValue(ParentB->GetPathName())})));
	Passed &= TestTrue(TEXT("Parent set v2 returns no handles"), Outcome.HandleResults.IsEmpty());
	Passed &= RequireExactChangedSet(*this, TEXT("Parent set v2"), Outcome,
		ParentSetV2Id, Blackboard->GetPathName(),
		FString::Printf(TEXT("asset:%s/Parent"), *Blackboard->GetPathName()));
	Passed &= TestEqual(TEXT("Parent set v2 captures both preimages before one footprint"),
		Transaction.Order, TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint")});
	Passed &= TestEqual(TEXT("Parent set v2 enlists exactly two footprint items"),
		Transaction.EnlistedFootprint.Num(), 2);
	if (Transaction.EnlistedFootprint.Num() == 2)
	{
		Passed &= TestEqual(TEXT("Parent set v2 Keys footprint is first"),
			Transaction.EnlistedFootprint[0].StableId,
			FString::Printf(TEXT("asset:%s/Keys[*]"), *Blackboard->GetPathName()));
		Passed &= TestEqual(TEXT("Parent set v2 Keys component is exact"),
			Transaction.EnlistedFootprint[0].RollbackComponentId,
			FString(TEXT("assetdocument.ai.blackboard.parent.derived_closure.v2")));
		Passed &= TestEqual(TEXT("Parent set v2 Parent footprint is second"),
			Transaction.EnlistedFootprint[1].StableId,
			FString::Printf(TEXT("asset:%s/Parent"), *Blackboard->GetPathName()));
		Passed &= TestEqual(TEXT("Parent set v2 Parent component is exact"),
			Transaction.EnlistedFootprint[1].RollbackComponentId,
			FString(TEXT("assetdocument.ai.blackboard.parent.reference.v2")));
	}
	Passed &= TestTrue(TEXT("Parent set v2 installs exact ParentB"), Blackboard->Parent == ParentB);
	const FBlackboard::FKey ParentBKey = Child->GetKeyID(TEXT("FromParentB"));
	Passed &= TestTrue(TEXT("Parent set v2 propagates the new ParentB projection to child"),
		Child->GetKey(ParentBKey) != nullptr);
	Passed &= TestTrue(TEXT("Parent set v2 actual change dirties target package"), Package->IsDirty());
	Passed &= TestTrue(TEXT("Parent set v2 outer Atomic rollback succeeds"),
		Transaction.Rollback(TEXT("Test.ParentSetV2Rollback")).bValid);
	Passed &= TestTrue(TEXT("Parent set v2 rollback records both reverse actions"),
		Transaction.Order.Num() >= 2);
	if (Transaction.Order.Num() >= 2)
	{
		Passed &= TestEqual(TEXT("Parent set v2 rollback restores Parent first"),
			Transaction.Order[Transaction.Order.Num() - 2],
			FString(TEXT("rollback:assetdocument.ai.blackboard.parent.reference.v2")));
		Passed &= TestEqual(TEXT("Parent set v2 rollback restores derived closure second"),
			Transaction.Order.Last(),
			FString(TEXT("rollback:assetdocument.ai.blackboard.parent.derived_closure.v2")));
	}
	Passed &= RequireObservation(*this, TEXT("Parent set v2 rollback restores target and dirty state"),
		Before, *Blackboard);
	Passed &= TestTrue(TEXT("Parent set v2 rollback restores child ParentA projection"),
		Child->GetKey(Child->GetKeyID(TEXT("FromParentA"))) != nullptr
			&& Child->GetKey(Child->GetKeyID(TEXT("FromParentB"))) == nullptr);

	Package->SetDirtyFlag(false);
	const FBlackboardObservation NoOpBefore(*Blackboard);
	FBlackboardLifecycleTransaction NoOpTransaction;
	FBlackboardLifecycleHandleResolver NoOpHandles(NoOpTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> NoOpHandle = ResolveAIMutableAsset(
		*this, *Blackboard, NoOpTransaction, NoOpHandles);
	if (!NoOpHandle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome NoOp = Provider->Invoke(
		MakeParentSetV2Invocation(NoOpHandle.GetValue(), *Blackboard, ParentA),
		NoOpHandles, &NoOpTransaction);
	Passed &= RequireSuccess(*this, TEXT("Parent set v2 same-parent no-op"), NoOp);
	Passed &= TestTrue(TEXT("Parent set v2 no-op returns exact ParentA DTO"),
		RuntimeValueEquals(NoOp.Value,
			ObjectValue({TEXT("parent_path_or_null")},
				{StringValue(ParentA->GetPathName())})));
	Passed &= TestTrue(TEXT("Parent set v2 no-op has empty handles and ChangedSet"),
		NoOp.HandleResults.IsEmpty() && NoOp.ChangedSet.IsEmpty());
	Passed &= TestTrue(TEXT("Parent set v2 no-op has zero capture and footprint"),
		NoOpTransaction.ActionEnlistCount == 0
			&& NoOpTransaction.FootprintEnlistCount == 0
			&& NoOpTransaction.EnlistedFootprint.IsEmpty());
	Passed &= TestTrue(TEXT("Parent set v2 no-op preserves Parent/projection/dirty"),
		NoOpBefore.Equals(*Blackboard) && !Package->IsDirty());

	FBlackboardLifecycleTransaction ClearTransaction;
	FBlackboardLifecycleHandleResolver ClearHandles(ClearTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> ClearHandle = ResolveAIMutableAsset(
		*this, *Blackboard, ClearTransaction, ClearHandles);
	if (!ClearHandle.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome Clear = Provider->Invoke(
		MakeParentSetV2Invocation(ClearHandle.GetValue(), *Blackboard, nullptr),
		ClearHandles, &ClearTransaction);
	Passed &= RequireSuccess(*this, TEXT("Parent set v2 clear Parent"), Clear);
	Passed &= TestTrue(TEXT("Parent set v2 clear returns exact null Parent DTO"),
		RuntimeValueEquals(Clear.Value,
			ObjectValue({TEXT("parent_path_or_null")}, {NullValue()})));
	Passed &= RequireExactChangedSet(*this, TEXT("Parent set v2 clear"), Clear,
		ParentSetV2Id, Blackboard->GetPathName(),
		FString::Printf(TEXT("asset:%s/Parent"), *Blackboard->GetPathName()));
	Passed &= TestTrue(TEXT("Parent set v2 clear removes Parent and child inherited projection"),
		Blackboard->Parent == nullptr
			&& Child->GetKey(Child->GetKeyID(TEXT("FromParentA"))) == nullptr);
	Passed &= TestTrue(TEXT("Parent set v2 clear dirties the target package"), Package->IsDirty());
	Passed &= TestTrue(TEXT("Parent set v2 clear rollback succeeds"),
		ClearTransaction.Rollback(TEXT("Test.ParentSetV2ClearRollback")).bValid);
	Passed &= RequireObservation(*this, TEXT("Parent set v2 clear rollback restores target/dirty"),
		NoOpBefore, *Blackboard);
	Passed &= TestTrue(TEXT("Parent set v2 clear rollback restores child inherited projection"),
		Child->GetKey(Child->GetKeyID(TEXT("FromParentA"))) != nullptr);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2ParentSetFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.ParentSet.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2ParentSetFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: invalid inputs/authority fail before write, and target
	//   identity drift after footprint fails before success with Atomic rollback.
	// - Candidate technique: error partitioning plus identity state transition.
	// - Risk or gap: final readback may prove only Parent value, while a missing
	//   exact Parent handle may be misclassified as a missing domain Parent.
	// - Test: real row-owned handle miss and post-footprint target rename.
	// - Expected observable outcome: AuthorityUnavailable before capture; target
	//   drift fails with empty outputs and rollback restores Parent/derived closure.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2ParentSetFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Parent = NewObject<UBlackboardData>(
		Package, TEXT("Parent"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* AlternateParent = NewObject<UBlackboardData>(
		Package, TEXT("AlternateParent"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardData* Child = NewObject<UBlackboardData>(
		Package, TEXT("Child"), RF_Public | RF_Standalone | RF_Transactional);
	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* NonBlackboard =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(Package);
	if (!TestNotNull(TEXT("Parent set failure target exists"), Blackboard)
		|| !TestNotNull(TEXT("Parent set failure Parent exists"), Parent)
		|| !TestNotNull(TEXT("Parent set failure alternate Parent exists"), AlternateParent)
		|| !TestNotNull(TEXT("Parent set failure child exists"), Child)
		|| !TestNotNull(TEXT("Parent set failure wrong-class object exists"), NonBlackboard)) return false;
	Blackboard->Keys.Reset(); Parent->Keys.Reset(); AlternateParent->Keys.Reset(); Child->Keys.Reset();
	AddExistingKey(*Blackboard, TEXT("TargetLocal"), 1);
	AddExistingKey(*Parent, TEXT("ParentLocal"), 2);
	AddExistingKey(*AlternateParent, TEXT("AlternateParentLocal"), 4);
	AddExistingKey(*Child, TEXT("ChildLocal"), 3);
	Blackboard->Parent = Parent;
	Blackboard->UpdateParentKeys(); Blackboard->UpdateKeyIDs();
	AlternateParent->UpdateParentKeys(); AlternateParent->UpdateKeyIDs();
	Child->Parent = Blackboard;
	Child->UpdateParentKeys(); Child->UpdateKeyIDs();
	Package->SetDirtyFlag(false);
	const FBlackboardObservation Before(*Blackboard);
	bool Passed = true;
	auto RunCase = [&](const FString& Label,
		TFunctionRef<void(FAssetDocumentPrimitiveInvocation&)> Mutate,
		const TCHAR* ExpectedDiagnostic,
		bool bProvideTransaction = true,
		const TCHAR* ExpectedDetailMessage = nullptr)
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) { Passed = false; return; }
		FAssetDocumentPrimitiveInvocation Invocation = MakeParentSetV2Invocation(
			Handle.GetValue(), *Blackboard, Parent);
		Mutate(Invocation);
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			Invocation, Handles, bProvideTransaction ? &Transaction : nullptr);
		Passed &= RequireNoForwardWrite(*this, Label, Outcome, Before, *Blackboard, Transaction);
		Passed &= TestEqual(*(Label + TEXT(" has exactly one diagnostic")),
			Outcome.Diagnostics.Num(), 1);
		if (Outcome.Diagnostics.Num() == 1)
		{
			Passed &= TestEqual(*(Label + TEXT(" uses its exact frozen terminal")),
				Outcome.Diagnostics[0].Code, FString(ExpectedDiagnostic));
			if (ExpectedDetailMessage != nullptr)
			{
				Passed &= TestEqual(*(Label + TEXT(" preserves only the lower detail message")),
					Outcome.Diagnostics[0].Message, FString(ExpectedDetailMessage));
			}
		}
	};
	RunCase(TEXT("Parent set v2 rejects malformed DTO"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments = ObjectValue({}, {});
		}, TEXT("AIProvider.InvalidValueShape"));
	RunCase(TEXT("Parent set v2 rejects wrong phase"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
		}, TEXT("AIProvider.InvalidContext"));
	RunCase(TEXT("Parent set v2 rejects missing active transaction"),
		[](FAssetDocumentPrimitiveInvocation&) {},
		TEXT("AIProvider.InvalidTransaction"), false);
	RunCase(TEXT("Parent set v2 rejects missing mutable asset HA"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.HandleArguments.Reset();
		}, TEXT("AIProvider.InvalidHandle"));
	RunCase(TEXT("Parent set v2 rejects request identity drift"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.RequestId = FGuid::NewGuid();
		}, TEXT("AIProvider.InvalidTransaction"));
	RunCase(TEXT("Parent set v2 rejects revision drift"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			++Invocation.RuntimeRevision;
		}, TEXT("AIProvider.InvalidTransaction"));
	RunCase(TEXT("Parent set v2 rejects stale HA slot"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.HandleArguments[0].Handle.Slot += 999;
		}, TEXT("AIProvider.InvalidHandle"));
	RunCase(TEXT("Parent set v2 rejects wrong HA provider"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.HandleArguments[0].Handle.ProviderId = TEXT("editor");
		}, TEXT("AIProvider.InvalidHandle"), true,
		TEXT("Handle provider does not match the validation context."));
	RunCase(TEXT("Parent set v2 rejects target identity mismatch"),
		[Parent](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments.Elements[0].StringValue = Parent->GetPathName();
		}, TEXT("AIProvider.InvalidHandle"));
	RunCase(TEXT("Parent set v2 rejects unavailable desired Parent"),
		[](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments.Elements[1].StringValue =
				TEXT("/Temp/AssetDocumentAI/Missing.Missing");
		}, TEXT("AIProvider.Blackboard.ParentUnavailable"));
	RunCase(TEXT("Parent set v2 rejects wrong-class desired Parent"),
		[NonBlackboard](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments.Elements[1].StringValue = NonBlackboard->GetPathName();
		}, TEXT("AIProvider.Blackboard.ParentClassMismatch"));
	RunCase(TEXT("Parent set v2 rejects exact self cycle"),
		[Blackboard](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments.Elements[1].StringValue = Blackboard->GetPathName();
		}, TEXT("AIProvider.Blackboard.ParentCycle"));
	RunCase(TEXT("Parent set v2 rejects descendant cycle through public IsChildOf"),
		[Child](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			Invocation.ValueArguments.Elements[1].StringValue = Child->GetPathName();
		}, TEXT("AIProvider.Blackboard.ParentCycle"));
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *NonBlackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeParentSetV2Invocation(Handle.GetValue(), *NonBlackboard, Parent),
			Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, TEXT("Parent set v2 rejects wrong-class target"),
			Outcome, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("Parent set v2 wrong-class target uses exact terminal"),
			HasDiagnostic(Outcome, TEXT("AIProvider.Blackboard.ParentClassMismatch")));
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const bool bPreviousEditor = GIsEditor;
		GIsEditor = false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeParentSetV2Invocation(Handle.GetValue(), *Blackboard, AlternateParent),
			Handles, &Transaction);
		GIsEditor = bPreviousEditor;
		Passed &= RequireNoForwardWrite(*this, TEXT("Parent set v2 rejects non-Editor context"),
			Outcome, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("Parent set v2 non-Editor context uses exact terminal"),
			HasDiagnostic(Outcome, TEXT("AIProvider.InvalidContext")));
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint FailurePoint(
				EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2BeforePropertyRows);
			Outcome = Provider->Invoke(
				MakeParentSetV2Invocation(Handle.GetValue(), *Blackboard, AlternateParent),
				Handles, &Transaction);
		}
		Passed &= RequireNoForwardWrite(*this,
			TEXT("Parent set v2 rejects unavailable Parent PropertyEditor authority"),
			Outcome, Before, *Blackboard, Transaction);
		Passed &= TestTrue(TEXT("Parent set v2 PropertyEditor failure uses exact terminal"),
			HasDiagnostic(Outcome, TEXT("AIProvider.AuthorityUnavailable")));
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint FailurePoint(
				EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2MissingPropertyHandle);
			Outcome = Provider->Invoke(
				MakeParentSetV2Invocation(Handle.GetValue(), *Blackboard, AlternateParent),
				Handles, &Transaction);
		}
		Passed &= RequireNoForwardWrite(*this,
			TEXT("Parent set v2 rejects missing exact Parent property handle"),
			Outcome, Before, *Blackboard, Transaction);
		Passed &= TestEqual(TEXT("Parent set v2 missing handle has one diagnostic"),
			Outcome.Diagnostics.Num(), 1);
		if (Outcome.Diagnostics.Num() == 1)
		{
			Passed &= TestEqual(TEXT("Parent set v2 missing handle uses authority terminal"),
				Outcome.Diagnostics[0].Code, FString(TEXT("AIProvider.AuthorityUnavailable")));
		}
		Passed &= TestTrue(TEXT("Parent set v2 missing handle has zero capture and footprint"),
			Transaction.ActionEnlistCount == 0
				&& Transaction.FootprintEnlistCount == 0
				&& Transaction.EnlistedFootprint.IsEmpty());
	}

	auto RunEnlistmentFailure = [&](const FString& Label, const int32 RejectActionAtCall,
		const int32 RejectFootprintAtCall, const TCHAR* ExpectedDetailMessage)
	{
		FBlackboardLifecycleTransaction Transaction;
		Transaction.RejectActionAtCall = RejectActionAtCall;
		Transaction.RejectFootprintAtCall = RejectFootprintAtCall;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) { Passed = false; return; }
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeParentSetV2Invocation(Handle.GetValue(), *Blackboard, AlternateParent),
			Handles, &Transaction);
		Passed &= RequireNoForwardWrite(*this, Label, Outcome, Before, *Blackboard, Transaction);
		Passed &= TestEqual(*(Label + TEXT(" has exactly one diagnostic")),
			Outcome.Diagnostics.Num(), 1);
		if (Outcome.Diagnostics.Num() == 1)
		{
			Passed &= TestEqual(*(Label + TEXT(" normalizes to its sole frozen terminal")),
				Outcome.Diagnostics[0].Code, FString(TEXT("AIProvider.OperationRejected")));
			Passed &= TestEqual(*(Label + TEXT(" preserves only the lower detail message")),
				Outcome.Diagnostics[0].Message, FString(ExpectedDetailMessage));
		}
	};
	RunEnlistmentFailure(TEXT("Parent set v2 rejects local-closure action enlistment"),
		0, INDEX_NONE, TEXT("The active transaction rejected an AI typed rollback action."));
	RunEnlistmentFailure(TEXT("Parent set v2 rejects Parent-reference action enlistment"),
		1, INDEX_NONE, TEXT("The active transaction rejected a typed rollback action."));
	RunEnlistmentFailure(TEXT("Parent set v2 rejects the complete ordered footprint"),
		INDEX_NONE, 0, TEXT("Test transaction deliberately rejected this enlistment."));

	auto RunPostFootprintFailure = [&](const FString& Label,
		const EAssetDocumentAIBlackboardTestFailurePoint FailurePoint,
		const TCHAR* ExpectedDiagnostic)
	{
		Package->SetDirtyFlag(false);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) { Passed = false; return; }
		FAssetDocumentPrimitiveOutcome Outcome;
		{
			FScopedBlackboardTestFailurePoint ScopedFailurePoint(FailurePoint);
			Outcome = Provider->Invoke(
				MakeParentSetV2Invocation(Handle.GetValue(), *Blackboard, AlternateParent),
				Handles, &Transaction);
		}
		Passed &= TestEqual(*(Label + TEXT(" fails")), Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		Passed &= TestTrue(*(Label + TEXT(" uses its closed terminal")),
			HasDiagnostic(Outcome, ExpectedDiagnostic));
		Passed &= TestTrue(*(Label + TEXT(" publishes no failure outputs")),
			Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
		Passed &= TestEqual(*(Label + TEXT(" retains exact action/footprint order")),
			Transaction.Order,
			TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint")});
		Passed &= TestTrue(*(Label + TEXT(" outer Atomic rollback succeeds")),
			Transaction.Rollback(TEXT("Test.ParentSetV2InjectedFailure")).bValid);
		Passed &= RequireObservation(*this, Label + TEXT(" rollback restores Parent and dirty state"),
			Before, *Blackboard);
		Passed &= TestTrue(*(Label + TEXT(" rollback restores the child derived closure")),
			Child->GetKey(Child->GetKeyID(TEXT("ParentLocal"))) != nullptr
				&& Child->GetKey(Child->GetKeyID(TEXT("AlternateParentLocal"))) == nullptr);
	};
	RunPostFootprintFailure(TEXT("Parent set v2 injected SetValue rejection"),
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2BeforeSetValue,
		TEXT("AIProvider.Blackboard.ParentWriteRejected"));
	RunPostFootprintFailure(TEXT("Parent set v2 injected after SetValue"),
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2AfterSetValue,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	RunPostFootprintFailure(TEXT("Parent set v2 injected after propagation"),
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2AfterPropagation,
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"));
	RunPostFootprintFailure(TEXT("Parent set v2 injected Parent readback failure"),
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2BeforeReadback,
		TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"));
	RunPostFootprintFailure(TEXT("Parent set v2 injected ChangedSet mismatch"),
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2ChangedSetMismatch,
		TEXT("AIProvider.Blackboard.ParentChangedSetMismatch"));
	{
		Package->SetDirtyFlag(false);
		const FString OriginalName = Blackboard->GetName();
		const FString OriginalPath = Blackboard->GetPathName();
		FBlackboardLifecycleTransaction Transaction;
		Transaction.RenameAfterFootprint = Blackboard;
		Transaction.RenameAfterFootprintTo = FString::Printf(
			TEXT("BlackboardIdentityDrift_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const TOptional<FAssetDocumentOpaqueHandle> Handle = ResolveAIMutableAsset(
			*this, *Blackboard, Transaction, Handles);
		if (!Handle.IsSet()) return false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeParentSetV2Invocation(Handle.GetValue(), *Blackboard, AlternateParent),
			Handles, &Transaction);
		Passed &= TestTrue(TEXT("Parent set v2 target identity seam performs a real rename"),
			Transaction.bRenameAfterFootprintSucceeded
				&& Blackboard->GetPathName() != OriginalPath);
		Passed &= TestEqual(TEXT("Parent set v2 rejects final target identity drift"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		Passed &= TestTrue(TEXT("Parent set v2 target identity drift uses invalid-handle terminal"),
			HasDiagnostic(Outcome, TEXT("AIProvider.InvalidHandle")));
		Passed &= TestTrue(TEXT("Parent set v2 target identity drift publishes no outputs"),
			Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
		Passed &= TestEqual(TEXT("Parent set v2 target identity drift retains mutation order"),
			Transaction.Order,
			TArray<FString>{TEXT("action"), TEXT("action"), TEXT("footprint")});
		Passed &= TestTrue(TEXT("Parent set v2 test restores the original target identity"),
			Blackboard->Rename(*OriginalName, nullptr,
				REN_DontCreateRedirectors));
		Passed &= TestTrue(TEXT("Parent set v2 target identity drift Atomic rollback succeeds"),
			Transaction.Rollback(TEXT("Test.ParentSetV2TargetIdentityDrift")).bValid);
		Passed &= TestTrue(TEXT("Parent set v2 target identity drift rollback restores Parent"),
			Blackboard->Parent == Parent);
		Passed &= TestTrue(TEXT("Parent set v2 target identity drift rollback restores child closure"),
			Child->GetKey(Child->GetKeyID(TEXT("ParentLocal"))) != nullptr
				&& Child->GetKey(Child->GetKeyID(TEXT("AlternateParentLocal"))) == nullptr);
		if (Blackboard->Parent != Parent)
		{
			Blackboard->Parent = Parent;
			Blackboard->UpdateParentKeys();
			Blackboard->UpdateKeyIDs();
			Child->UpdateParentKeys();
			Child->UpdateKeyIDs();
		}
		Package->SetDirtyFlag(false);
		Passed &= TestEqual(TEXT("Parent set v2 target path is restored after the test"),
			Blackboard->GetPathName(), OriginalPath);
		Passed &= RequireObservation(*this,
			TEXT("Parent set v2 target identity rollback restores target state"),
			Before, *Blackboard);
	}
	Passed &= TestTrue(TEXT("Parent set v2 pre-write failures preserve dirty state"),
		!Package->IsDirty() && Before.Equals(*Blackboard));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2LocalKeysReadbackTest,
	"UEAssetDoc.AIProvider.BlackboardV2.LocalKeysReadback.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2LocalKeysReadbackTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: owned local names are returned in stored local order.
	// - Candidate technique: equivalence partitioning for non-empty and empty local lists.
	// - Risk or gap: sorting or inherited-name aggregation can appear canonical.
	// - Test: an intentionally non-lexicographic local sequence.
	// - Expected observable outcome: exact ordered_local_key_names with no mutation.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2LocalKeysReadback_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("local-keys readback Blackboard fixture exists"), Blackboard)) return false;
	Blackboard->Keys.Reset();
	for (const FName Name : {FName(TEXT("Zulu")), FName(TEXT("Alpha")), FName(TEXT("Middle"))})
	{
		FBlackboardEntry& Entry = Blackboard->Keys.AddDefaulted_GetRef();
		Entry.EntryName = Name;
	}
	Package->SetDirtyFlag(false);
	const FBlackboardObservation Before(*Blackboard);
	FBlackboardLifecycleTransaction Transaction;
	FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
	bool Passed = RequireCanonicalLocalKeysReadback(*this, Provider->Invoke(
		MakeLocalKeysReadbackInvocation(*Blackboard, Transaction.GetMutationToken()), Handles, nullptr),
		{TEXT("Zulu"), TEXT("Alpha"), TEXT("Middle")});
	Passed &= TestTrue(TEXT("local-keys readback leaves no transaction footprint or mutation"),
		Transaction.EnlistedFootprint.IsEmpty() && Before.Equals(*Blackboard) && !Package->IsDirty());
	Blackboard->Keys.Reset();
	FBlackboardLifecycleTransaction EmptyTransaction;
	FBlackboardLifecycleHandleResolver EmptyHandles(EmptyTransaction.GetMutationToken());
	Passed &= RequireCanonicalLocalKeysReadback(*this, Provider->Invoke(
		MakeLocalKeysReadbackInvocation(*Blackboard, EmptyTransaction.GetMutationToken()), EmptyHandles, nullptr), {});
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardV2LocalKeysReadbackFailureTest,
	"UEAssetDoc.AIProvider.BlackboardV2.LocalKeysReadback.Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardV2LocalKeysReadbackFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	// Test design:
	// - Behavior under test: all invalid local-name observations fail closed.
	// - Candidate technique: error partitioning across DTO, phase, handle,
	//   transaction, exact-target identity and local-name validity.
	// - Risk or gap: a read-only descriptor might admit HA/Atomic or emit a value.
	// - Test: representative invalid inputs and malformed stored evidence.
	// - Expected observable outcome: empty output, no footprint and stable terminal code.
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BlackboardV2LocalKeysReadbackFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* NonBlackboard =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(Package);
	if (!TestNotNull(TEXT("local-keys failure Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("local-keys failure non-Blackboard fixture exists"), NonBlackboard)) return false;
	Package->SetDirtyFlag(false);
	const FBlackboardObservation Before(*Blackboard);
	bool Passed = true;
	auto RequireFailure = [&](const TCHAR* Label, FAssetDocumentPrimitiveInvocation Invocation,
		IAssetDocumentAtomicTransaction* ActiveTransaction, const TCHAR* Diagnostic = nullptr)
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		Invocation.RequestId = Transaction.GetMutationToken().RequestId;
		Invocation.TransactionId = Transaction.GetMutationToken().TransactionId;
		Invocation.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			Invocation, Handles, ActiveTransaction == nullptr ? nullptr : &Transaction);
		Passed &= RequireReadbackFailure(*this, Label, Outcome, Before, *Blackboard,
			Transaction, Diagnostic);
	};
	{
		FBlackboardLifecycleTransaction Token;
		RequireFailure(TEXT("local-keys readback rejects Apply"),
			MakeLocalKeysReadbackInvocation(*Blackboard, Token.GetMutationToken(),
				EAssetDocumentRuntimeRequestPhase::Apply), nullptr);
		FAssetDocumentPrimitiveInvocation Malformed =
			MakeLocalKeysReadbackInvocation(*Blackboard, Token.GetMutationToken());
		Malformed.ValueArguments = ObjectValue({}, {});
		RequireFailure(TEXT("local-keys readback rejects malformed request"), MoveTemp(Malformed), nullptr,
			TEXT("AIProvider.InvalidValueShape"));
		FAssetDocumentPrimitiveInvocation WithHandle =
			MakeLocalKeysReadbackInvocation(*Blackboard, Token.GetMutationToken());
		FAssetDocumentOpaqueHandle Unexpected;
		Unexpected.Kind = EAssetDocumentHandleKind::Asset;
		Unexpected.Access = EAssetDocumentHandleAccess::Mutable;
		WithHandle.HandleArguments = {{TEXT("asset"), Unexpected}};
		RequireFailure(TEXT("local-keys readback rejects supplied handle"), MoveTemp(WithHandle), nullptr,
			TEXT("AIProvider.InvalidHandle"));
		FAssetDocumentPrimitiveInvocation Missing =
			MakeLocalKeysReadbackInvocation(*Blackboard, Token.GetMutationToken());
		Missing.ValueArguments.Elements[0].StringValue = TEXT("/Temp/AssetDocumentAI/Missing.Missing");
		RequireFailure(TEXT("local-keys readback rejects unavailable target"), MoveTemp(Missing), nullptr,
			TEXT("AIProvider.Blackboard.LocalKeysReadbackUnavailable"));
		RequireFailure(TEXT("local-keys readback rejects non-Blackboard target"),
			MakeLocalKeysReadbackInvocation(*NonBlackboard, Token.GetMutationToken()), nullptr,
			TEXT("AIProvider.Blackboard.LocalKeysReadbackUnavailable"));
	}
	{
		FBlackboardLifecycleTransaction Active;
		FBlackboardLifecycleHandleResolver Handles(Active.GetMutationToken());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeLocalKeysReadbackInvocation(*Blackboard, Active.GetMutationToken()), Handles, &Active);
		Passed &= RequireReadbackFailure(*this, TEXT("local-keys readback rejects active transaction"),
			Outcome, Before, *Blackboard, Active, TEXT("AIProvider.InvalidTransaction"));
	}
	{
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		const bool bPreviousEditor = GIsEditor;
		GIsEditor = false;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeLocalKeysReadbackInvocation(*Blackboard, Transaction.GetMutationToken()), Handles, nullptr);
		GIsEditor = bPreviousEditor;
		Passed &= RequireReadbackFailure(*this, TEXT("local-keys readback rejects non-Editor context"),
			Outcome, Before, *Blackboard, Transaction, TEXT("AIProvider.InvalidContext"));
	}
	Blackboard->Keys.AddDefaulted_GetRef().EntryName = NAME_None;
	{
		const FBlackboardObservation InvalidBefore(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		Passed &= RequireReadbackFailure(*this, TEXT("local-keys readback rejects NAME_None"), Provider->Invoke(
			MakeLocalKeysReadbackInvocation(*Blackboard, Transaction.GetMutationToken()), Handles, nullptr),
			InvalidBefore, *Blackboard, Transaction, TEXT("AIProvider.Blackboard.LocalKeyNameInvalid"));
	}
	Blackboard->Keys.Reset();
	Blackboard->Keys.AddDefaulted_GetRef().EntryName = TEXT("Duplicate");
	Blackboard->Keys.AddDefaulted_GetRef().EntryName = TEXT("Duplicate");
	{
		const FBlackboardObservation DuplicateBefore(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		FBlackboardLifecycleHandleResolver Handles(Transaction.GetMutationToken());
		Passed &= RequireReadbackFailure(*this, TEXT("local-keys readback rejects duplicate FName identity"), Provider->Invoke(
			MakeLocalKeysReadbackInvocation(*Blackboard, Transaction.GetMutationToken()), Handles, nullptr),
			DuplicateBefore, *Blackboard, Transaction, TEXT("AIProvider.Blackboard.DuplicateKey"));
	}
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardKeyTypeStructCanonicalWriteTest,
	"UEAssetDoc.AIProvider.BlackboardKeyTypeStructCanonicalWrite.BalancedLifecycleAndRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardKeyTypeStructCanonicalWriteTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	using namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite;
	// SC-WR-001 PrepareBoundedLiteralDependencies: the generic Runtime DTO may be
	// canonical while the Struct WRITE wrapper is not.  That grammar refusal must
	// be InvalidDto before any native target or transaction state can change.
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/StructCanonicalWriteInvalidDto_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardKeyType_Struct* KeyType = NewObject<UBlackboardKeyType_Struct>(
		Blackboard, TEXT("StructKeyType"), RF_Transactional);
	if (!TestNotNull(TEXT("SC-WR-001 exact Blackboard fixture exists"), Blackboard)
		|| !TestNotNull(TEXT("SC-WR-001 exact stock Struct target exists"), KeyType)) return false;
	KeyType->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	FAssetDocumentAIBlackboardCanonicalStructNested& OriginalValue =
		KeyType->DefaultValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>();
	OriginalValue.Count = 13;
	OriginalValue.Label = TEXT("preflight-unchanged");
	FBlackboardEntry Entry;
	Entry.EntryName = TEXT("StructTarget");
	Entry.KeyType = KeyType;
	Blackboard->Keys.Add(Entry);
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Package->SetDirtyFlag(false);

	const FAssetDocumentRuntimeValueDto InvalidWriteGrammar = ObjectValue(
		{TEXT("NotDefaultValue")},
		{ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({TEXT("Count"), TEXT("Label")},
				{IntegerValue(41), StringValue(TEXT("not-a-write-wrapper"))}),
				StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())})});
	const UBlackboardKeyType_Struct* const OriginalTarget = KeyType;
	const UScriptStruct* const OriginalSchema = KeyType->DefaultValue.GetScriptStruct();
	const void* const OriginalMemory = KeyType->DefaultValue.GetMemory();
	FBlackboardLifecycleTransaction Transaction;

	const FPrepareResult Prepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
		*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), KeyType, Transaction,
		InvalidWriteGrammar);
	const FAssetDocumentAIBlackboardCanonicalStructNested* ResultValue =
		KeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>();
	bool Passed = TestTrue(TEXT("SC-WR-001 invalid wrapper remains a generic canonical Runtime DTO"),
		IsAssetDocumentAICanonicalRuntimeValue(InvalidWriteGrammar))
		&& TestFalse(TEXT("SC-WR-001 invalid wrapper preflight does not succeed"), Prepared.bSucceeded)
		&& TestEqual(TEXT("SC-WR-001 invalid wrapper reports InvalidDto"), Prepared.Failure,
			EFailure::InvalidDto)
		&& TestFalse(TEXT("SC-WR-001 invalid wrapper returns no prepared value"),
			Prepared.Prepared.IsValid())
		&& TestEqual(TEXT("SC-WR-001 preserves the exact native Struct target pointer"),
			static_cast<const UBlackboardKeyType_Struct*>(KeyType), OriginalTarget)
		&& TestEqual(TEXT("SC-WR-001 preserves the exact native Struct schema"),
			KeyType->DefaultValue.GetScriptStruct(), OriginalSchema)
		&& TestEqual(TEXT("SC-WR-001 preserves the exact native Struct memory"),
			static_cast<const void*>(KeyType->DefaultValue.GetMemory()), OriginalMemory)
		&& TestTrue(TEXT("SC-WR-001 preserves the exact native Struct value"),
			ResultValue != nullptr && ResultValue->Count == 13
				&& ResultValue->Label == TEXT("preflight-unchanged"))
		&& TestEqual(TEXT("SC-WR-001 captures no rollback action"), Transaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("SC-WR-001 enlists no mutation footprint"),
			Transaction.FootprintEnlistCount, 0)
		&& TestTrue(TEXT("SC-WR-001 records no transaction action or footprint order"),
			Transaction.Order.IsEmpty())
		&& TestTrue(TEXT("SC-WR-001 leaves no completed footprint"),
			Transaction.EnlistedFootprint.IsEmpty())
		&& TestFalse(TEXT("SC-WR-001 leaves the package clean"), Package->IsDirty());

	// SC-WR-001 RootStructType: a syntactically canonical wrapper whose top-level
	// Struct literal resolves to a UClass must close as DependencyUnavailable
	// before scratch/readback, capture, transaction residue, or native mutation.
	const FAssetDocumentRuntimeValueDto RootStructTypeDesired = ObjectValue(
		{TEXT("DefaultValue")},
		{ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({}, {}), StringValue(UObject::StaticClass()->GetPathName())})});
	FBlackboardLifecycleTransaction RootStructTypeTransaction;
	const FPrepareResult RootStructTypePrepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
		*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), KeyType, RootStructTypeTransaction,
		RootStructTypeDesired);
	const FAssetDocumentAIBlackboardCanonicalStructNested* RootStructTypeValue =
		KeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Passed &= TestTrue(TEXT("SC-WR-001 root Struct type remains a generic canonical Runtime DTO"),
		IsAssetDocumentAICanonicalRuntimeValue(RootStructTypeDesired))
		&& TestFalse(TEXT("SC-WR-001 root Struct type preflight does not succeed"),
			RootStructTypePrepared.bSucceeded)
		&& TestEqual(TEXT("SC-WR-001 root Struct type reports DependencyUnavailable"),
			RootStructTypePrepared.Failure, EFailure::DependencyUnavailable)
		&& TestFalse(TEXT("SC-WR-001 root Struct type returns no prepared value"),
			RootStructTypePrepared.Prepared.IsValid())
		&& TestEqual(TEXT("SC-WR-001 root Struct type preserves the exact native Struct target pointer"),
			static_cast<const UBlackboardKeyType_Struct*>(KeyType), OriginalTarget)
		&& TestEqual(TEXT("SC-WR-001 root Struct type preserves the exact native Struct schema"),
			KeyType->DefaultValue.GetScriptStruct(), OriginalSchema)
		&& TestEqual(TEXT("SC-WR-001 root Struct type preserves the exact native Struct memory"),
			static_cast<const void*>(KeyType->DefaultValue.GetMemory()), OriginalMemory)
		&& TestTrue(TEXT("SC-WR-001 root Struct type preserves the exact native Struct value"),
			RootStructTypeValue != nullptr && RootStructTypeValue->Count == 13
				&& RootStructTypeValue->Label == TEXT("preflight-unchanged"))
		&& TestEqual(TEXT("SC-WR-001 root Struct type captures no rollback action"),
			RootStructTypeTransaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("SC-WR-001 root Struct type enlists no mutation footprint"),
			RootStructTypeTransaction.FootprintEnlistCount, 0)
		&& TestTrue(TEXT("SC-WR-001 root Struct type records no transaction action or footprint order"),
			RootStructTypeTransaction.Order.IsEmpty())
		&& TestTrue(TEXT("SC-WR-001 root Struct type leaves no completed footprint"),
			RootStructTypeTransaction.EnlistedFootprint.IsEmpty())
		&& TestFalse(TEXT("SC-WR-001 root Struct type leaves the package clean"), Package->IsDirty());

	// SC-WR-001 dependency/schema characterization uses this complete,
	// hand-written admitted DTO as its only oracle.  Individual cells copy it
	// and locate the changed field by ObjectKeys; none asks production READ to
	// manufacture an expected input.
	UEnum* const CanonicalStructEnum = StaticEnum<EAssetDocumentAIBlackboardCanonicalStructTestEnum>();
	if (!TestNotNull(TEXT("SC-WR-001 canonical Enum fixture exists"), CanonicalStructEnum)) return false;
	const FAssetDocumentRuntimeValueDto AdmittedDesired = ObjectValue(
		{TEXT("DefaultValue")},
		{ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({TEXT("ArrayValues"), TEXT("BoolValue"), TEXT("ByteValue"),
				TEXT("ClassValue"), TEXT("DoubleValue"), TEXT("DynamicValue"),
				TEXT("EnumValue"), TEXT("FloatValue"), TEXT("HardObject"),
				TEXT("Int64Value"), TEXT("IntValue"), TEXT("MapValues"), TEXT("NameValue"),
				TEXT("NestedValue"), TEXT("SetValues"), TEXT("SoftClassValue"),
				TEXT("SoftObjectValue"), TEXT("StaticValues"), TEXT("StringValue"),
				TEXT("TextValue"), TEXT("UInt16Value"), TEXT("UInt32Value"),
				TEXT("UInt64Value")},
				{ArrayValue({}), BooleanValue(false), IntegerValue(0),
					ReferenceValue(TEXT("ClassRef"), *UObject::StaticClass()),
					NumberValue(0.0), NullValue(),
					ObjectValue({TEXT("Enum"), TEXT("Enumerator")},
						{StringValue(CanonicalStructEnum->GetPathName()), StringValue(TEXT("First"))}),
					NumberValue(0.0), ReferenceValue(TEXT("AssetRef"), *Blackboard),
					IntegerValue(0), IntegerValue(0), ArrayValue({}),
					StringValue(TEXT("")),
					ObjectValue({TEXT("Properties"), TEXT("Struct")},
						{ObjectValue({TEXT("Count"), TEXT("Label")},
							{IntegerValue(0), StringValue(TEXT(""))}),
							StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())}),
					ArrayValue({}),
					ObjectValue({TEXT("Kind"), TEXT("Path")},
						{StringValue(TEXT("SoftClassRef")), StringValue(UObject::StaticClass()->GetPathName())}),
					ObjectValue({TEXT("Kind"), TEXT("Path")},
						{StringValue(TEXT("SoftAssetRef")), StringValue(Blackboard->GetPathName())}),
					ArrayValue({IntegerValue(0), IntegerValue(0), IntegerValue(0)}),
					StringValue(TEXT("")), StringValue(TEXT("")), IntegerValue(0), IntegerValue(0),
					ObjectValue({TEXT("Kind"), TEXT("Value")},
						{StringValue(TEXT("UInt64")), StringValue(TEXT("0"))})}),
				StringValue(FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct()->GetPathName())})});
	const auto FindObjectValue = [](FAssetDocumentRuntimeValueDto& Object, const TCHAR* Key)
		-> FAssetDocumentRuntimeValueDto*
	{
		const int32 Index = Object.ObjectKeys.IndexOfByKey(FString(Key));
		return Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
	};
	const auto FindAdmittedField = [&](FAssetDocumentRuntimeValueDto& Desired, const TCHAR* Field)
		-> FAssetDocumentRuntimeValueDto*
	{
		FAssetDocumentRuntimeValueDto* const DefaultValue = FindObjectValue(Desired, TEXT("DefaultValue"));
		FAssetDocumentRuntimeValueDto* const Properties =
			DefaultValue == nullptr ? nullptr : FindObjectValue(*DefaultValue, TEXT("Properties"));
		return Properties == nullptr ? nullptr : FindObjectValue(*Properties, Field);
	};
	const auto SetPath = [&](FAssetDocumentRuntimeValueDto& Reference, const FString& Path)
	{
		FAssetDocumentRuntimeValueDto* const PathValue = FindObjectValue(Reference, TEXT("Path"));
		if (PathValue == nullptr) return false;
		*PathValue = StringValue(Path);
		return true;
	};
	const auto SetObjectString = [&](FAssetDocumentRuntimeValueDto& Object, const TCHAR* Key, const FString& Value)
	{
		FAssetDocumentRuntimeValueDto* const StringField = FindObjectValue(Object, Key);
		if (StringField == nullptr) return false;
		*StringField = StringValue(Value);
		return true;
	};
	const auto SetStructPath = [&](FAssetDocumentRuntimeValueDto& StructValue, const FString& Path)
	{
		FAssetDocumentRuntimeValueDto* const PathValue = FindObjectValue(StructValue, TEXT("Struct"));
		if (PathValue == nullptr) return false;
		*PathValue = StringValue(Path);
		return true;
	};
	const auto RequirePreflightNoResidue = [&](const FString& Label, const FPrepareResult& Result,
		const EFailure ExpectedFailure, const FBlackboardLifecycleTransaction& CaseTransaction,
		const FBlackboardObservation& Before)
	{
		const FAssetDocumentAIBlackboardCanonicalStructNested* const LiveValue =
			KeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>();
		return TestFalse(*(Label + TEXT(" does not prepare")), Result.bSucceeded)
			&& TestEqual(*(Label + TEXT(" reports the exact terminal")), Result.Failure, ExpectedFailure)
			&& TestFalse(*(Label + TEXT(" returns an invalid prepared value")), Result.Prepared.IsValid())
			&& TestEqual(*(Label + TEXT(" preserves the exact native Struct target pointer")),
				static_cast<const UBlackboardKeyType_Struct*>(KeyType), OriginalTarget)
			&& TestEqual(*(Label + TEXT(" preserves the exact native Struct schema")),
				KeyType->DefaultValue.GetScriptStruct(), OriginalSchema)
			&& TestEqual(*(Label + TEXT(" preserves the exact native Struct memory")),
				static_cast<const void*>(KeyType->DefaultValue.GetMemory()), OriginalMemory)
			&& TestTrue(*(Label + TEXT(" preserves the live nested Struct value")),
				LiveValue != nullptr && LiveValue->Count == 13 && LiveValue->Label == TEXT("preflight-unchanged"))
			&& TestEqual(*(Label + TEXT(" captures zero rollback actions")), CaseTransaction.ActionEnlistCount, 0)
			&& TestEqual(*(Label + TEXT(" enlists zero mutation footprints")), CaseTransaction.FootprintEnlistCount, 0)
			&& TestTrue(*(Label + TEXT(" records zero action or footprint order")), CaseTransaction.Order.IsEmpty())
			&& TestTrue(*(Label + TEXT(" leaves zero completed footprint")), CaseTransaction.EnlistedFootprint.IsEmpty())
			&& TestFalse(*(Label + TEXT(" leaves the package clean")), Package->IsDirty())
			&& TestTrue(*(Label + TEXT(" leaves the live target unchanged")), Before.Equals(*Blackboard));
	};
	const auto RequireDistinctMissingHardAttempt = [&](const FString& Label,
		const TArray<FDependencyAttemptAudit>& Attempts, const FString& MissingPath)
	{
		TSet<FString> DistinctPaths;
		const FDependencyAttemptAudit* MissingAttempt = nullptr;
		bool bEveryLiteralIsDistinct = true;
		for (const FDependencyAttemptAudit& Attempt : Attempts)
		{
			bEveryLiteralIsDistinct &= !DistinctPaths.Contains(Attempt.LiteralPath);
			DistinctPaths.Add(Attempt.LiteralPath);
			if (Attempt.LiteralPath == MissingPath) MissingAttempt = &Attempt;
		}
		return TestTrue(*(Label + TEXT(" attempts each distinct hard literal at most once")),
			bEveryLiteralIsDistinct)
			&& TestNotNull(*(Label + TEXT(" records the missing hard literal")), MissingAttempt)
			&& (MissingAttempt == nullptr ||
				(TestTrue(*(Label + TEXT(" tries one load for the missing hard literal")), MissingAttempt->bTriedLoad)
					&& TestFalse(*(Label + TEXT(" records no successful load for the missing hard literal")),
						MissingAttempt->bLoadSucceeded)));
	};
	const auto RunMissingHardDependencyCase = [&](const FString& Label,
		const FAssetDocumentRuntimeValueDto& Desired, const FString& MissingPath)
	{
		Package->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction CaseTransaction;
		ResetDependencyAttemptAudit();
		ResetScratchInitializeAudit();
		const FPrepareResult Result = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
			*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), KeyType, CaseTransaction, Desired);
		const TArray<FDependencyAttemptAudit> Attempts = GetDependencyAttemptAudit();
		return TestTrue(*(Label + TEXT(" remains a generic canonical Runtime DTO")),
			IsAssetDocumentAICanonicalRuntimeValue(Desired))
			&& RequirePreflightNoResidue(Label, Result, EFailure::DependencyUnavailable, CaseTransaction, Before)
			&& TestEqual(*(Label + TEXT(" rejects before any scratch initialization")),
				GetScratchInitializeAuditCount(), 0)
			&& RequireDistinctMissingHardAttempt(Label, Attempts, MissingPath);
	};

	// SC-WR-001 EnumExactIdentity: a complete admitted literal whose Enum
	// dependency resolves to a wrong loaded type remains a dependency refusal.
	FAssetDocumentRuntimeValueDto EnumExactIdentityDesired = AdmittedDesired;
	FAssetDocumentRuntimeValueDto* const EnumExactIdentity =
		FindAdmittedField(EnumExactIdentityDesired, TEXT("EnumValue"));
	if (!TestNotNull(TEXT("SC-WR-001 finds EnumValue by ObjectKeys"), EnumExactIdentity)
		|| !TestTrue(TEXT("SC-WR-001 rewrites only the Enum dependency path"),
			EnumExactIdentity != nullptr && SetObjectString(*EnumExactIdentity, TEXT("Enum"),
				UObject::StaticClass()->GetPathName()))) return false;
	Package->SetDirtyFlag(false);
	const FBlackboardObservation EnumExactIdentityBefore(*Blackboard);
	FBlackboardLifecycleTransaction EnumExactIdentityTransaction;
	const FPrepareResult EnumExactIdentityPrepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
		*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), KeyType, EnumExactIdentityTransaction,
		EnumExactIdentityDesired);
	const FAssetDocumentAIBlackboardCanonicalStructNested* EnumExactIdentityValue =
		KeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Passed &= TestTrue(TEXT("SC-WR-001 enum exact identity remains a generic canonical Runtime DTO"),
		IsAssetDocumentAICanonicalRuntimeValue(EnumExactIdentityDesired))
		&& TestFalse(TEXT("SC-WR-001 enum exact identity preflight does not succeed"),
			EnumExactIdentityPrepared.bSucceeded)
		&& TestEqual(TEXT("SC-WR-001 enum exact identity reports DependencyUnavailable"),
			EnumExactIdentityPrepared.Failure, EFailure::DependencyUnavailable)
		&& TestFalse(TEXT("SC-WR-001 enum exact identity returns no prepared value"),
			EnumExactIdentityPrepared.Prepared.IsValid())
		&& TestEqual(TEXT("SC-WR-001 enum exact identity preserves the exact native Struct target pointer"),
			static_cast<const UBlackboardKeyType_Struct*>(KeyType), OriginalTarget)
		&& TestEqual(TEXT("SC-WR-001 enum exact identity preserves the exact native Struct schema"),
			KeyType->DefaultValue.GetScriptStruct(), OriginalSchema)
		&& TestEqual(TEXT("SC-WR-001 enum exact identity preserves the exact native Struct memory"),
			static_cast<const void*>(KeyType->DefaultValue.GetMemory()), OriginalMemory)
		&& TestTrue(TEXT("SC-WR-001 enum exact identity preserves the exact native Struct value"),
			EnumExactIdentityValue != nullptr && EnumExactIdentityValue->Count == 13
				&& EnumExactIdentityValue->Label == TEXT("preflight-unchanged"))
		&& TestEqual(TEXT("SC-WR-001 enum exact identity captures no rollback action"),
			EnumExactIdentityTransaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("SC-WR-001 enum exact identity enlists no mutation footprint"),
			EnumExactIdentityTransaction.FootprintEnlistCount, 0)
		&& TestTrue(TEXT("SC-WR-001 enum exact identity records no transaction action or footprint order"),
			EnumExactIdentityTransaction.Order.IsEmpty())
		&& TestTrue(TEXT("SC-WR-001 enum exact identity leaves no completed footprint"),
			EnumExactIdentityTransaction.EnlistedFootprint.IsEmpty())
		&& TestFalse(TEXT("SC-WR-001 enum exact identity leaves the package clean"), Package->IsDirty())
		&& TestTrue(TEXT("SC-WR-001 enum exact identity leaves the live target unchanged"),
			EnumExactIdentityBefore.Equals(*Blackboard));

	const FString MissingRootPath = TEXT("/Game/AssetDocumentTests/BlackboardStructCanonicalWrite/MissingRoot.MissingRoot");
	// The four deliberately missing hard literals below each reach TryLoad once.
	// Keep the expected Engine warnings scoped to their fixed test-only family. Older engines also warn that the
	// missing package was skipped; newer ones resolve the object's package quietly.
#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	AddExpectedError(
		TEXT("LoadPackage: SkipPackage: /Game/AssetDocumentTests/BlackboardStructCanonicalWrite/Missing"),
		EAutomationExpectedErrorFlags::Contains, 4);
#endif
	AddExpectedError(
		TEXT("Failed to find object 'Object /Game/AssetDocumentTests/BlackboardStructCanonicalWrite/Missing"),
		EAutomationExpectedErrorFlags::Contains, 4);
	FAssetDocumentRuntimeValueDto MissingRootDesired = AdmittedDesired;
	FAssetDocumentRuntimeValueDto* const MissingRootStruct = FindObjectValue(MissingRootDesired, TEXT("DefaultValue"));
	if (!TestNotNull(TEXT("SC-WR-001 finds root Struct wrapper by ObjectKeys"), MissingRootStruct)
		|| !TestTrue(TEXT("SC-WR-001 rewrites only the missing root Struct path"),
			MissingRootStruct != nullptr && SetStructPath(*MissingRootStruct, MissingRootPath))) return false;
	Passed &= RunMissingHardDependencyCase(TEXT("SC-WR-001 missing canonical root Struct path"),
		MissingRootDesired, MissingRootPath);

	const FString MissingClassPath = TEXT("/Game/AssetDocumentTests/BlackboardStructCanonicalWrite/MissingClass.MissingClass_C");
	FAssetDocumentRuntimeValueDto MissingClassDesired = AdmittedDesired;
	FAssetDocumentRuntimeValueDto* const MissingClass = FindAdmittedField(MissingClassDesired, TEXT("ClassValue"));
	if (!TestNotNull(TEXT("SC-WR-001 finds ClassValue by ObjectKeys"), MissingClass)
		|| !TestTrue(TEXT("SC-WR-001 rewrites only the missing hard Class path"),
			MissingClass != nullptr && SetPath(*MissingClass, MissingClassPath))) return false;
	Passed &= RunMissingHardDependencyCase(TEXT("SC-WR-001 missing hard Class path"),
		MissingClassDesired, MissingClassPath);

	const FString MissingObjectPath = TEXT("/Game/AssetDocumentTests/BlackboardStructCanonicalWrite/MissingObject.MissingObject");
	FAssetDocumentRuntimeValueDto MissingObjectDesired = AdmittedDesired;
	FAssetDocumentRuntimeValueDto* const MissingObject = FindAdmittedField(MissingObjectDesired, TEXT("HardObject"));
	if (!TestNotNull(TEXT("SC-WR-001 finds HardObject by ObjectKeys"), MissingObject)
		|| !TestTrue(TEXT("SC-WR-001 rewrites only the missing hard Object path"),
			MissingObject != nullptr && SetPath(*MissingObject, MissingObjectPath))) return false;
	Passed &= RunMissingHardDependencyCase(TEXT("SC-WR-001 missing hard Object path"),
		MissingObjectDesired, MissingObjectPath);

	const FString MissingEnumPath = TEXT("/Game/AssetDocumentTests/BlackboardStructCanonicalWrite/MissingEnum.MissingEnum");
	FAssetDocumentRuntimeValueDto MissingEnumDesired = AdmittedDesired;
	FAssetDocumentRuntimeValueDto* const MissingEnum = FindAdmittedField(MissingEnumDesired, TEXT("EnumValue"));
	if (!TestNotNull(TEXT("SC-WR-001 finds exact Enum by ObjectKeys"), MissingEnum)
		|| !TestTrue(TEXT("SC-WR-001 rewrites only the missing exact Enum path"),
			MissingEnum != nullptr && SetObjectString(*MissingEnum, TEXT("Enum"), MissingEnumPath))) return false;
	Passed &= RunMissingHardDependencyCase(TEXT("SC-WR-001 missing exact Enum path"),
		MissingEnumDesired, MissingEnumPath);

	FAssetDocumentRuntimeValueDto NestedSchemaDriftDesired = AdmittedDesired;
	FAssetDocumentRuntimeValueDto* const NestedSchemaDrift =
		FindAdmittedField(NestedSchemaDriftDesired, TEXT("NestedValue"));
	if (!TestNotNull(TEXT("SC-WR-001 finds nested static Struct wrapper by ObjectKeys"), NestedSchemaDrift)
		|| !TestTrue(TEXT("SC-WR-001 rewrites only the nested static Struct path"),
			NestedSchemaDrift != nullptr && SetStructPath(*NestedSchemaDrift,
				FAssetDocumentAIBlackboardCanonicalStructArrayOnly::StaticStruct()->GetPathName()))) return false;
	Package->SetDirtyFlag(false);
	const FBlackboardObservation NestedSchemaDriftBefore(*Blackboard);
	FBlackboardLifecycleTransaction NestedSchemaDriftTransaction;
	const FPrepareResult NestedSchemaDriftPrepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
		*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), KeyType, NestedSchemaDriftTransaction,
		NestedSchemaDriftDesired);
	if (NestedSchemaDriftPrepared.Failure != EFailure::SchemaDrift)
	{
		AddInfo(FString::Printf(
			TEXT("SC-WR-001 nested static Struct schema drift terminal: actual=%d expected=%d"),
			static_cast<int32>(NestedSchemaDriftPrepared.Failure), static_cast<int32>(EFailure::SchemaDrift)));
	}
	Passed &= TestTrue(TEXT("SC-WR-001 nested static Struct schema drift remains canonical"),
		IsAssetDocumentAICanonicalRuntimeValue(NestedSchemaDriftDesired))
		&& RequirePreflightNoResidue(TEXT("SC-WR-001 nested static Struct schema drift"),
			NestedSchemaDriftPrepared, EFailure::SchemaDrift, NestedSchemaDriftTransaction,
			NestedSchemaDriftBefore);

	const FString MissingSoftClassPath = TEXT("/Game/AssetDocumentTests/BlackboardStructCanonicalWrite/MissingSoftClass.MissingSoftClass_C");
	const FString MissingSoftObjectPath = TEXT("/Game/AssetDocumentTests/BlackboardStructCanonicalWrite/MissingSoftObject.MissingSoftObject");
	FAssetDocumentRuntimeValueDto MissingSoftDependenciesDesired = AdmittedDesired;
	FAssetDocumentRuntimeValueDto* const MissingSoftClass =
		FindAdmittedField(MissingSoftDependenciesDesired, TEXT("SoftClassValue"));
	FAssetDocumentRuntimeValueDto* const MissingSoftObject =
		FindAdmittedField(MissingSoftDependenciesDesired, TEXT("SoftObjectValue"));
	if (!TestNotNull(TEXT("SC-WR-001 finds SoftClassValue by ObjectKeys"), MissingSoftClass)
		|| !TestNotNull(TEXT("SC-WR-001 finds SoftObjectValue by ObjectKeys"), MissingSoftObject)
		|| !TestTrue(TEXT("SC-WR-001 rewrites only canonical soft dependency paths"),
			MissingSoftClass != nullptr && MissingSoftObject != nullptr
				&& SetPath(*MissingSoftClass, MissingSoftClassPath)
				&& SetPath(*MissingSoftObject, MissingSoftObjectPath))) return false;
	Package->SetDirtyFlag(false);
	const FBlackboardObservation MissingSoftDependenciesBefore(*Blackboard);
	FBlackboardLifecycleTransaction MissingSoftDependenciesTransaction;
	ResetDependencyAttemptAudit();
	const FPrepareResult MissingSoftDependenciesPrepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
		*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), KeyType, MissingSoftDependenciesTransaction,
		MissingSoftDependenciesDesired);
	const TArray<FDependencyAttemptAudit> MissingSoftDependencyAttempts = GetDependencyAttemptAudit();
	if (!MissingSoftDependenciesPrepared.bSucceeded)
	{
		AddInfo(FString::Printf(
			TEXT("SC-WR-001 missing soft paths Prepare failure: actual=%d prepare_audit=%s"),
			static_cast<int32>(MissingSoftDependenciesPrepared.Failure),
			GetPrepareAuditTerminalName(GetPrepareAuditTerminal())));
		for (const FDependencyAttemptAudit& Attempt : MissingSoftDependencyAttempts)
		{
			AddInfo(FString::Printf(
				TEXT("SC-WR-001 missing soft paths dependency audit: path=%s resolve=%d tryload=%d result=%d"),
				*Attempt.LiteralPath, Attempt.bResolveObjectSucceeded ? 1 : 0,
				Attempt.bTriedLoad ? 1 : 0, Attempt.bLoadSucceeded ? 1 : 0));
		}
	}
	const TSet<FString> ExpectedHardDependencyPaths{
		FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct()->GetPathName(),
		UObject::StaticClass()->GetPathName(), CanonicalStructEnum->GetPathName(), Blackboard->GetPathName(),
		FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName()};
	TSet<FString> SoftCaseDistinctPaths;
	bool bSoftCaseOnlyAuditsDistinctHardPaths = true;
	for (const FDependencyAttemptAudit& Attempt : MissingSoftDependencyAttempts)
	{
		bSoftCaseOnlyAuditsDistinctHardPaths &= ExpectedHardDependencyPaths.Contains(Attempt.LiteralPath)
			&& !SoftCaseDistinctPaths.Contains(Attempt.LiteralPath);
		SoftCaseDistinctPaths.Add(Attempt.LiteralPath);
	}
	Passed &= TestTrue(TEXT("SC-WR-001 missing canonical soft paths remain a generic canonical Runtime DTO"),
		IsAssetDocumentAICanonicalRuntimeValue(MissingSoftDependenciesDesired))
		&& TestTrue(TEXT("SC-WR-001 missing canonical soft paths prepare successfully"),
			MissingSoftDependenciesPrepared.bSucceeded)
		&& TestEqual(TEXT("SC-WR-001 missing canonical soft paths report no failure"),
			MissingSoftDependenciesPrepared.Failure, EFailure::None)
		&& TestTrue(TEXT("SC-WR-001 missing canonical soft paths return a prepared value"),
			MissingSoftDependenciesPrepared.Prepared.IsValid())
		&& TestTrue(TEXT("SC-WR-001 missing canonical soft paths never Resolve or TryLoad a soft literal"),
			!MissingSoftDependencyAttempts.ContainsByPredicate([&](const FDependencyAttemptAudit& Attempt)
			{
				return Attempt.LiteralPath == MissingSoftClassPath || Attempt.LiteralPath == MissingSoftObjectPath;
			}))
		&& TestTrue(TEXT("SC-WR-001 missing canonical soft paths audit only distinct hard/root/nested/enum literals"),
			bSoftCaseOnlyAuditsDistinctHardPaths)
		&& TestEqual(TEXT("SC-WR-001 missing canonical soft paths capture zero rollback actions"),
			MissingSoftDependenciesTransaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("SC-WR-001 missing canonical soft paths enlist zero mutation footprints"),
			MissingSoftDependenciesTransaction.FootprintEnlistCount, 0)
		&& TestTrue(TEXT("SC-WR-001 missing canonical soft paths record zero action or footprint order"),
			MissingSoftDependenciesTransaction.Order.IsEmpty())
		&& TestTrue(TEXT("SC-WR-001 missing canonical soft paths leave zero completed footprint"),
			MissingSoftDependenciesTransaction.EnlistedFootprint.IsEmpty())
		&& TestFalse(TEXT("SC-WR-001 missing canonical soft paths leave the package clean"), Package->IsDirty())
		&& TestTrue(TEXT("SC-WR-001 missing canonical soft paths leave the live target unchanged"),
			MissingSoftDependenciesBefore.Equals(*Blackboard));

	// SC-WR-001 finite preflight budgets: these dev-only precharges characterize
	// the exact remaining work needed by small hand-written literals. They cannot
	// alter request selection or shipping limits, and every terminal remains a
	// zero-action preflight outcome.
	const FAssetDocumentRuntimeValueDto NestedBudgetDesired = ObjectValue({TEXT("DefaultValue")},
		{ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({TEXT("Count"), TEXT("Label")},
				{IntegerValue(41), StringValue(TEXT("budget-desired"))}),
				StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())})});
	const FAssetDocumentRuntimeValueDto ArrayBudgetDesired = ObjectValue({TEXT("DefaultValue")},
		{ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({TEXT("Values")}, {ArrayValue({IntegerValue(7)})}),
				StringValue(FAssetDocumentAIBlackboardCanonicalStructArrayOnly::StaticStruct()->GetPathName())})});
	const auto PrepareWithControl = [&](const FPrepareTestControl& Control,
		const FAssetDocumentRuntimeValueDto& Desired, FBlackboardLifecycleTransaction& Transaction)
	{
		FScopedPrepareTestControl ScopedControl(Control);
		return PrepareCanonicalWrite(ECaller::UpdateExistingV2, *Blackboard,
			*UBlackboardKeyType_Struct::StaticClass(), KeyType, Transaction, Desired);
	};
	const auto RequireBudgetPrepared = [&](const FString& Label, const FPrepareResult& Result,
		const FBlackboardLifecycleTransaction& Transaction, const FBlackboardObservation& Before)
	{
		return TestTrue(*(Label + TEXT(" prepares a valid scratch")), Result.bSucceeded)
			&& TestEqual(*(Label + TEXT(" reports no failure")), Result.Failure, EFailure::None)
			&& TestTrue(*(Label + TEXT(" returns a valid prepared value")), Result.Prepared.IsValid())
			&& TestEqual(*(Label + TEXT(" captures no rollback action")), Transaction.ActionEnlistCount, 0)
			&& TestEqual(*(Label + TEXT(" enlists no mutation footprint")), Transaction.FootprintEnlistCount, 0)
			&& TestTrue(*(Label + TEXT(" records no action or footprint order")), Transaction.Order.IsEmpty())
			&& TestTrue(*(Label + TEXT(" leaves no completed footprint")), Transaction.EnlistedFootprint.IsEmpty())
			&& TestFalse(*(Label + TEXT(" leaves the package clean")), Package->IsDirty())
			&& TestTrue(*(Label + TEXT(" leaves the live target unchanged")), Before.Equals(*Blackboard));
	};
	const auto RequireBudgetFailure = [&](const FString& Label, const FPrepareResult& Result,
		const FBlackboardLifecycleTransaction& Transaction, const FBlackboardObservation& Before,
		const EFailure ExpectedFailure)
	{
		return TestFalse(*(Label + TEXT(" does not prepare")), Result.bSucceeded)
			&& TestEqual(*(Label + TEXT(" reports the exact terminal")), Result.Failure, ExpectedFailure)
			&& TestFalse(*(Label + TEXT(" returns no prepared value")), Result.Prepared.IsValid())
			&& TestEqual(*(Label + TEXT(" captures no rollback action")), Transaction.ActionEnlistCount, 0)
			&& TestEqual(*(Label + TEXT(" enlists no mutation footprint")), Transaction.FootprintEnlistCount, 0)
			&& TestTrue(*(Label + TEXT(" records no action or footprint order")), Transaction.Order.IsEmpty())
			&& TestTrue(*(Label + TEXT(" leaves no completed footprint")), Transaction.EnlistedFootprint.IsEmpty())
			&& TestFalse(*(Label + TEXT(" leaves the package clean")), Package->IsDirty())
			&& TestTrue(*(Label + TEXT(" leaves the live target unchanged")), Before.Equals(*Blackboard));
	};
	const auto PrepareWithoutControl = [&](const FAssetDocumentRuntimeValueDto& Desired,
		FBlackboardLifecycleTransaction& Transaction)
	{
		return PrepareCanonicalWrite(ECaller::UpdateExistingV2, *Blackboard,
			*UBlackboardKeyType_Struct::StaticClass(), KeyType, Transaction, Desired);
	};
	const auto RunDirectPrepareCase = [&](const FString& Label,
		const FAssetDocumentRuntimeValueDto& Desired, const EFailure ExpectedFailure)
	{
		Package->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		const FPrepareResult Result = PrepareWithoutControl(Desired, Transaction);
		return ExpectedFailure == EFailure::None
			? RequireBudgetPrepared(Label, Result, Transaction, Before)
			: RequireBudgetFailure(Label, Result, Transaction, Before, ExpectedFailure);
	};
	const auto RunRejectedEnumCase = [&](const FString& Label, const FString& Enumerator)
	{
		FAssetDocumentRuntimeValueDto Desired = AdmittedDesired;
		FAssetDocumentRuntimeValueDto* const EnumValue = FindAdmittedField(Desired, TEXT("EnumValue"));
		if (!TestNotNull(*(Label + TEXT(" finds EnumValue")), EnumValue)
			|| !TestTrue(*(Label + TEXT(" rewrites only Enumerator")),
				EnumValue != nullptr && SetObjectString(*EnumValue, TEXT("Enumerator"), Enumerator))) return false;
		ResetScratchInitializeAudit();
		return RunDirectPrepareCase(Label, Desired, EFailure::InvalidDto)
			&& TestEqual(*(Label + TEXT(" rejects before any scratch initialization")),
				GetScratchInitializeAuditCount(), 0);
	};
	Passed &= RunRejectedEnumCase(TEXT("WR001.Hidden Enum is InvalidDto"), TEXT("HiddenValue"));
	Passed &= RunRejectedEnumCase(TEXT("WR001.MAX Enum is InvalidDto"), TEXT("MAX"));

	FAssetDocumentRuntimeValueDto ComparisonAdmissionDesired = AdmittedDesired;
	FAssetDocumentRuntimeValueDto* const ComparisonSet =
		FindAdmittedField(ComparisonAdmissionDesired, TEXT("SetValues"));
	if (!TestNotNull(TEXT("WR001.ComparisonAdmission finds SetValues"), ComparisonSet)) return false;
	TArray<FAssetDocumentRuntimeValueDto> ComparisonStrings;
	for (TCHAR Character = TCHAR('a'); Character <= TCHAR('e'); ++Character)
	{
		ComparisonStrings.Add(StringValue(FString::ChrN(220 * 1024, Character)));
	}
	*ComparisonSet = ArrayValue(MoveTemp(ComparisonStrings));
	ResetScratchInitializeAudit();
	Passed &= RunDirectPrepareCase(TEXT("WR001.ComparisonAdmission over 1MiB"),
		ComparisonAdmissionDesired, EFailure::DecodeFailed)
		&& TestEqual(TEXT("WR001.ComparisonAdmission rejects before any scratch initialization"),
			GetScratchInitializeAuditCount(), 0);

	// SC-WR-001 actual DTO bounds: the depth wrapper is hand-written so the
	// decoder sees the same root plus N dynamic FInstancedStruct chain used by
	// READ.  DecodeStruct begins at depth zero and each Next reaches depth +1;
	// therefore N=14 and N=15 are below/exact while N=16 is one over.
	const auto MakeDynamicDepthDesired = [](const int32 NestedCount)
	{
		FAssetDocumentRuntimeValueDto Value = NullValue();
		for (int32 Index = 0; Index <= NestedCount; ++Index)
		{
			Value = ObjectValue({TEXT("Properties"), TEXT("Struct")},
				{ObjectValue({TEXT("Next")}, {Value}),
					StringValue(FAssetDocumentAIBlackboardCanonicalStructDepth::StaticStruct()->GetPathName())});
		}
		return ObjectValue({TEXT("DefaultValue")}, {Value});
	};
	Passed &= RunDirectPrepareCase(TEXT("WR001.ActualDepth dynamic below limit"),
		MakeDynamicDepthDesired(14), EFailure::None);
	Passed &= RunDirectPrepareCase(TEXT("WR001.ActualDepth dynamic exact limit"),
		MakeDynamicDepthDesired(15), EFailure::None);
	Passed &= RunDirectPrepareCase(TEXT("WR001.ActualDepth dynamic one-over limit"),
		MakeDynamicDepthDesired(16), EFailure::DecodeFailed);

	// SC-WR-001 actual DTO bounds: ArrayOnly has one direct TArray property, so
	// these literal element counts exercise the WRITE decoder's native container
	// admission rather than the existing test-only precharge cells.
	const auto MakeArrayOnlyDesired = [](const int32 Count)
	{
		TArray<FAssetDocumentRuntimeValueDto> Values;
		Values.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index) Values.Add(IntegerValue(7));
		return ObjectValue({TEXT("DefaultValue")},
			{ObjectValue({TEXT("Properties"), TEXT("Struct")},
				{ObjectValue({TEXT("Values")}, {ArrayValue(MoveTemp(Values))}),
					StringValue(FAssetDocumentAIBlackboardCanonicalStructArrayOnly::StaticStruct()->GetPathName())})});
	};
	Passed &= RunDirectPrepareCase(TEXT("WR001.ActualContainer 1023 entries"),
		MakeArrayOnlyDesired(1023), EFailure::None);
	Passed &= RunDirectPrepareCase(TEXT("WR001.ActualContainer 1024 entries"),
		MakeArrayOnlyDesired(1024), EFailure::None);
	Passed &= RunDirectPrepareCase(TEXT("WR001.ActualContainer 1025 entries"),
		MakeArrayOnlyDesired(1025), EFailure::InvalidDto);

	// SC-WR-001 actual DTO bounds: mutate only the hand-written admitted String
	// field with ASCII, so FString length and UTF-8 byte length are identical.
	const auto RunStringByteCase = [&](const FString& Label, const int32 ByteCount,
		const EFailure ExpectedFailure)
	{
		FAssetDocumentRuntimeValueDto Desired = AdmittedDesired;
		FAssetDocumentRuntimeValueDto* const Field = FindAdmittedField(Desired, TEXT("StringValue"));
		if (!TestNotNull(*(Label + TEXT(" finds the admitted StringValue field")), Field)) return false;
		*Field = StringValue(FString::ChrN(ByteCount, TEXT('x')));
		return RunDirectPrepareCase(Label, Desired, ExpectedFailure);
	};
	Passed &= RunStringByteCase(TEXT("WR001.ActualString 256KiB minus one bytes"),
		256 * 1024 - 1, EFailure::None);
	Passed &= RunStringByteCase(TEXT("WR001.ActualString 256KiB exact bytes"),
		256 * 1024, EFailure::None);
	Passed &= RunStringByteCase(TEXT("WR001.ActualString 256KiB plus one bytes"),
		256 * 1024 + 1, EFailure::InvalidDto);

	// SC-WR-001 comparison-byte bounds reuse the exact canonical byte count
	// established by the READ boundary test for an empty ArrayOnly value.  The
	// scoped READ control is intentionally active across PrepareCanonicalWrite:
	// WRITE's detached scratch/readback join must surface the closed READ budget
	// as its own preflight terminal without an action, footprint, dirty package,
	// or target mutation.
	const int64 EmptyArrayCanonicalBytes = 85
		+ FAssetDocumentAIBlackboardCanonicalStructArrayOnly::StaticStruct()->GetPathName().Len();
	const FAssetDocumentRuntimeValueDto EmptyArrayOnlyDesired = MakeArrayOnlyDesired(0);
	const auto RunComparisonByteCase = [&](const FString& Label, const int64 Precharge,
		const EFailure ExpectedFailure)
	{
		Package->SetDirtyFlag(false);
		const FBlackboardObservation Before(*Blackboard);
		FBlackboardLifecycleTransaction Transaction;
		AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::FReadTestControl Control;
		Control.PrechargeComparisonBytes = Precharge;
		FPrepareResult Result;
		{
			AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::Private::FScopedReadTestControl Scope(Control);
			Result = PrepareWithoutControl(EmptyArrayOnlyDesired, Transaction);
		}
		return ExpectedFailure == EFailure::None
			? RequireBudgetPrepared(Label, Result, Transaction, Before)
			: RequireBudgetFailure(Label, Result, Transaction, Before, ExpectedFailure);
	};
	Passed &= RunComparisonByteCase(TEXT("WR001.ComparisonBytes below limit"),
		1024 * 1024 - EmptyArrayCanonicalBytes - 1, EFailure::None);
	Passed &= RunComparisonByteCase(TEXT("WR001.ComparisonBytes exact limit"),
		1024 * 1024 - EmptyArrayCanonicalBytes, EFailure::None);
	Passed &= RunComparisonByteCase(TEXT("WR001.ComparisonBytes one-over limit"),
		1024 * 1024 - EmptyArrayCanonicalBytes + 1, EFailure::ReadbackFailed);
	for (const TPair<FPrepareTestControl, EFailure>& BudgetCase : {
		TPair<FPrepareTestControl, EFailure>(FPrepareTestControl{254, 0, 0, 0}, EFailure::None),
		TPair<FPrepareTestControl, EFailure>(FPrepareTestControl{255, 0, 0, 0}, EFailure::DecodeFailed),
		TPair<FPrepareTestControl, EFailure>(FPrepareTestControl{0, 0, 4091, 0}, EFailure::None),
		TPair<FPrepareTestControl, EFailure>(FPrepareTestControl{0, 0, 4092, 0}, EFailure::DecodeFailed),
		TPair<FPrepareTestControl, EFailure>(FPrepareTestControl{0, 0, 0, 255}, EFailure::None),
		TPair<FPrepareTestControl, EFailure>(FPrepareTestControl{0, 0, 0, 256}, EFailure::DependencyUnavailable)})
	{
		Package->SetDirtyFlag(false);
		const FBlackboardObservation BudgetBefore(*Blackboard);
		FBlackboardLifecycleTransaction BudgetTransaction;
		const FPrepareResult BudgetResult = PrepareWithControl(
			BudgetCase.Key, NestedBudgetDesired, BudgetTransaction);
		const bool bExpectedSuccess = BudgetCase.Value == EFailure::None;
		const FString Label = BudgetCase.Key.FieldsPrecharge != 0
			? FString::Printf(TEXT("WR001.PrepareBudget fields remaining %d"), 256 - BudgetCase.Key.FieldsPrecharge)
			: BudgetCase.Key.NodesPrecharge != 0
				? FString::Printf(TEXT("WR001.PrepareBudget nodes remaining %d"), 4096 - BudgetCase.Key.NodesPrecharge)
				: FString::Printf(TEXT("WR001.PrepareBudget dependency remaining %d"), 256 - BudgetCase.Key.DependencyAttemptPrecharge);
		Passed &= bExpectedSuccess
			? RequireBudgetPrepared(Label, BudgetResult, BudgetTransaction, BudgetBefore)
			: RequireBudgetFailure(Label, BudgetResult, BudgetTransaction, BudgetBefore, BudgetCase.Value);
	}
	for (const TPair<FPrepareTestControl, EFailure>& BudgetCase : {
		TPair<FPrepareTestControl, EFailure>(FPrepareTestControl{0, 1023, 0, 0}, EFailure::None),
		TPair<FPrepareTestControl, EFailure>(FPrepareTestControl{0, 1024, 0, 0}, EFailure::DecodeFailed)})
	{
		Package->SetDirtyFlag(false);
		const FBlackboardObservation BudgetBefore(*Blackboard);
		FBlackboardLifecycleTransaction BudgetTransaction;
		const FPrepareResult BudgetResult = PrepareWithControl(
			BudgetCase.Key, ArrayBudgetDesired, BudgetTransaction);
		const FString Label = FString::Printf(TEXT("WR001.PrepareBudget aggregate/container remaining %d"),
			1024 - BudgetCase.Key.ContainerPrecharge);
		Passed &= BudgetCase.Value == EFailure::None
			? RequireBudgetPrepared(Label, BudgetResult, BudgetTransaction, BudgetBefore)
			: RequireBudgetFailure(Label, BudgetResult, BudgetTransaction, BudgetBefore, BudgetCase.Value);
	}

	// SC-WR-001 PrepareBoundedLiteralDependencies: root Resolve and DecodeStruct
	// receive the same valid literal, so the dependency cache must publish one
	// distinct test-only attempt without entering a load or live mutation path.
	Package->SetDirtyFlag(false);
	const FBlackboardObservation DependencyAttemptAuditBefore(*Blackboard);
	const FAssetDocumentRuntimeValueDto DependencyAttemptAuditDesired = ObjectValue({TEXT("DefaultValue")},
		{ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({TEXT("Count"), TEXT("Label")},
				{IntegerValue(41), StringValue(TEXT("dependency-audit-desired"))}),
				StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())})});
	FBlackboardLifecycleTransaction DependencyAttemptAuditTransaction;
	ResetDependencyAttemptAudit();
	const FPrepareResult DependencyAttemptAuditPrepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
		*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), KeyType, DependencyAttemptAuditTransaction,
		DependencyAttemptAuditDesired);
	const TArray<FDependencyAttemptAudit> DependencyAttempts = GetDependencyAttemptAudit();
	const FAssetDocumentAIBlackboardCanonicalStructNested* DependencyAttemptAuditResultValue =
		KeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Passed &= TestTrue(TEXT("WR001.DependencyAttemptAudit valid Prepare succeeds"), DependencyAttemptAuditPrepared.bSucceeded)
		&& TestEqual(TEXT("WR001.DependencyAttemptAudit valid Prepare reports no failure"),
			DependencyAttemptAuditPrepared.Failure, EFailure::None)
		&& TestTrue(TEXT("WR001.DependencyAttemptAudit valid Prepare returns a prepared value"),
			DependencyAttemptAuditPrepared.Prepared.IsValid());
	const bool bHasOneDistinctAttempt = TestEqual(
		TEXT("WR001.DependencyAttemptAudit root Resolve and DecodeStruct cache-fold to one distinct attempt"),
		DependencyAttempts.Num(), 1);
	Passed &= bHasOneDistinctAttempt;
	if (bHasOneDistinctAttempt)
	{
		const FDependencyAttemptAudit& Attempt = DependencyAttempts[0];
		Passed &= TestEqual(TEXT("WR001.DependencyAttemptAudit records the nested Struct literal path"),
			Attempt.LiteralPath, FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())
			&& TestTrue(TEXT("WR001.DependencyAttemptAudit records ResolveObject success"),
				Attempt.bResolveObjectSucceeded)
			&& TestFalse(TEXT("WR001.DependencyAttemptAudit records no TryLoad for the loaded nested Struct"),
				Attempt.bTriedLoad)
			&& TestFalse(TEXT("WR001.DependencyAttemptAudit records no TryLoad success without a TryLoad"),
				Attempt.bLoadSucceeded);
	}
	Passed &= TestEqual(TEXT("WR001.DependencyAttemptAudit captures no rollback action"),
		DependencyAttemptAuditTransaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("WR001.DependencyAttemptAudit enlists no mutation footprint"),
			DependencyAttemptAuditTransaction.FootprintEnlistCount, 0)
		&& TestTrue(TEXT("WR001.DependencyAttemptAudit records no action or footprint order"),
			DependencyAttemptAuditTransaction.Order.IsEmpty())
		&& TestTrue(TEXT("WR001.DependencyAttemptAudit leaves no completed footprint"),
			DependencyAttemptAuditTransaction.EnlistedFootprint.IsEmpty())
		&& TestFalse(TEXT("WR001.DependencyAttemptAudit leaves the package clean"), Package->IsDirty())
		&& TestTrue(TEXT("WR001.DependencyAttemptAudit leaves the live target unchanged"),
			DependencyAttemptAuditBefore.Equals(*Blackboard))
		&& TestTrue(TEXT("WR001.DependencyAttemptAudit preserves the live nested Struct value"),
			DependencyAttemptAuditResultValue != nullptr && DependencyAttemptAuditResultValue->Count == 13
				&& DependencyAttemptAuditResultValue->Label == TEXT("preflight-unchanged"));

	// SC-WR-001 characterizes the real literal-path budget: the root dynamic
	// UDS schema is one cached dependency, followed by 255 loaded hard object
	// leaves and a final absent hard object literal.  The 257th distinct DTO
	// literal must fail before it is audited, loaded, or allowed to reach a
	// scratch value.  This package is intentionally never saved or discovered.
	const FString PathBudgetPackageName = FString::Printf(
		TEXT("/Game/AssetDocumentTests/BlackboardStructCanonicalWrite/PathBudget_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* const PathBudgetPackage = CreatePackage(*PathBudgetPackageName);
	UUserDefinedStruct* const PathBudgetStruct = FStructureEditorUtils::CreateUserDefinedStruct(
		PathBudgetPackage, TEXT("PathBudgetStruct"), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("WR001.PathBudget creates a test-owned UDS"), PathBudgetStruct)) return false;
	FProperty* PathBudgetInitialProperty = FStructureEditorUtils::GetPropertyByFriendlyName(
		PathBudgetStruct, TEXT("MemberVar_0"));
	if (!TestNotNull(TEXT("WR001.PathBudget finds the UDS default property"), PathBudgetInitialProperty)) return false;
	const FGuid PathBudgetPropertyGuid = FStructureEditorUtils::GetGuidForProperty(PathBudgetInitialProperty);
	const FEdGraphPinType PathBudgetObjectArrayType(
		FName(TEXT("object")), NAME_None, UDataAsset::StaticClass(), EPinContainerType::Array,
		false, FEdGraphTerminalType());
	if (!TestTrue(TEXT("WR001.PathBudget finds the UDS default property GUID"),
			PathBudgetPropertyGuid.IsValid())
		|| !TestTrue(TEXT("WR001.PathBudget changes the only UDS field to UDataAsset array"),
			FStructureEditorUtils::ChangeVariableType(
				PathBudgetStruct, PathBudgetPropertyGuid, PathBudgetObjectArrayType))
		|| !TestTrue(TEXT("WR001.PathBudget renames the only UDS field"),
			FStructureEditorUtils::RenameVariable(
				PathBudgetStruct, PathBudgetPropertyGuid, TEXT("ObjectPaths")))) return false;
	FStructureEditorUtils::CompileStructure(PathBudgetStruct);
	const FArrayProperty* const PathBudgetArrayProperty = CastField<FArrayProperty>(
		FStructureEditorUtils::GetPropertyByFriendlyName(PathBudgetStruct, TEXT("ObjectPaths")));
	const FObjectProperty* const PathBudgetElementProperty = PathBudgetArrayProperty == nullptr
		? nullptr : CastField<FObjectProperty>(PathBudgetArrayProperty->Inner);
	if (!TestNotNull(TEXT("WR001.PathBudget compiles the only UDS array field"), PathBudgetArrayProperty)
		|| !TestNotNull(TEXT("WR001.PathBudget compiles UDS array elements as hard object references"),
			PathBudgetElementProperty)
		|| !TestEqual(TEXT("WR001.PathBudget preserves the UDataAsset hard-reference class"),
			PathBudgetElementProperty == nullptr ? nullptr : PathBudgetElementProperty->PropertyClass.Get(),
			UDataAsset::StaticClass())) return false;

	TArray<TStrongObjectPtr<UBlackboardData>> PathBudgetAssets;
	TArray<FAssetDocumentRuntimeValueDto> PathBudgetReferences;
	TSet<FString> PathBudgetInputPaths;
	PathBudgetAssets.Reserve(255);
	PathBudgetReferences.Reserve(256);
	PathBudgetInputPaths.Reserve(257);
	PathBudgetInputPaths.Add(PathBudgetStruct->GetPathName());
	for (int32 Index = 0; Index != 255; ++Index)
	{
		UBlackboardData* const Asset = NewObject<UBlackboardData>(PathBudgetPackage,
			*FString::Printf(TEXT("HardObject_%03d"), Index), RF_Public | RF_Standalone);
		if (!TestNotNull(*FString::Printf(TEXT("WR001.PathBudget creates hard UDataAsset %d"), Index), Asset))
		{
			return false;
		}
		PathBudgetAssets.Emplace(Asset);
		PathBudgetReferences.Add(ReferenceValue(TEXT("AssetRef"), *Asset));
		PathBudgetInputPaths.Add(Asset->GetPathName());
	}
	const FString MissingPathBudgetObjectPath = PathBudgetPackageName + TEXT(".MissingHardObject_256");
	PathBudgetReferences.Add(ObjectValue({TEXT("Kind"), TEXT("Path")},
		{StringValue(TEXT("AssetRef")), StringValue(MissingPathBudgetObjectPath)}));
	PathBudgetInputPaths.Add(MissingPathBudgetObjectPath);
	const FAssetDocumentRuntimeValueDto PathBudgetDesired = ObjectValue({TEXT("DefaultValue")},
		{ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({TEXT("ObjectPaths")}, {ArrayValue(MoveTemp(PathBudgetReferences))}),
				StringValue(PathBudgetStruct->GetPathName())})});
	if (!TestTrue(TEXT("WR001.PathBudget desired DTO remains canonical"),
			IsAssetDocumentAICanonicalRuntimeValue(PathBudgetDesired))
		|| !TestEqual(TEXT("WR001.PathBudget input contains exactly 257 distinct DTO literals"),
			PathBudgetInputPaths.Num(), 257)) return false;

	Package->SetDirtyFlag(false);
	PathBudgetPackage->SetDirtyFlag(false);
	const FBlackboardObservation PathBudgetBefore(*Blackboard);
	FBlackboardLifecycleTransaction PathBudgetTransaction;
	ResetDependencyAttemptAudit();
	const FPrepareResult PathBudgetPrepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
		*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), KeyType, PathBudgetTransaction,
		PathBudgetDesired);
	const TArray<FDependencyAttemptAudit> PathBudgetAttempts = GetDependencyAttemptAudit();
	TSet<FString> PathBudgetAuditedPaths;
	bool bPathBudgetAttemptsAreDistinctLoadedInputs = true;
	for (const FDependencyAttemptAudit& Attempt : PathBudgetAttempts)
	{
		bPathBudgetAttemptsAreDistinctLoadedInputs &= PathBudgetInputPaths.Contains(Attempt.LiteralPath)
			&& Attempt.LiteralPath != MissingPathBudgetObjectPath
			&& !PathBudgetAuditedPaths.Contains(Attempt.LiteralPath)
			&& Attempt.bResolveObjectSucceeded && !Attempt.bTriedLoad && !Attempt.bLoadSucceeded;
		PathBudgetAuditedPaths.Add(Attempt.LiteralPath);
	}
	Passed &= TestFalse(TEXT("WR001.PathBudget 257th literal does not prepare"), PathBudgetPrepared.bSucceeded)
		&& TestEqual(TEXT("WR001.PathBudget 257th literal is DependencyUnavailable"),
			PathBudgetPrepared.Failure, EFailure::DependencyUnavailable)
		&& TestFalse(TEXT("WR001.PathBudget 257th literal returns no prepared value"),
			PathBudgetPrepared.Prepared.IsValid())
		&& TestEqual(TEXT("WR001.PathBudget audits exactly root plus 255 hard object paths"),
			PathBudgetAttempts.Num(), 256)
		&& TestEqual(TEXT("WR001.PathBudget audit paths stay one-to-one"),
			PathBudgetAuditedPaths.Num(), 256)
		&& TestTrue(TEXT("WR001.PathBudget audits only distinct resolved hard literals before the cap"),
			bPathBudgetAttemptsAreDistinctLoadedInputs)
		&& TestFalse(TEXT("WR001.PathBudget does not audit the capped missing 257th literal"),
			PathBudgetAuditedPaths.Contains(MissingPathBudgetObjectPath))
		&& TestEqual(TEXT("WR001.PathBudget captures zero rollback actions"),
			PathBudgetTransaction.ActionEnlistCount, 0)
		&& TestEqual(TEXT("WR001.PathBudget enlists zero mutation footprints"),
			PathBudgetTransaction.FootprintEnlistCount, 0)
		&& TestTrue(TEXT("WR001.PathBudget records zero action or footprint order"),
			PathBudgetTransaction.Order.IsEmpty())
		&& TestTrue(TEXT("WR001.PathBudget leaves zero completed footprint"),
			PathBudgetTransaction.EnlistedFootprint.IsEmpty())
		&& TestFalse(TEXT("WR001.PathBudget leaves the live target package clean"), Package->IsDirty())
		&& TestFalse(TEXT("WR001.PathBudget leaves the test-owned path package clean"), PathBudgetPackage->IsDirty())
		&& TestTrue(TEXT("WR001.PathBudget leaves the live target unchanged"),
			PathBudgetBefore.Equals(*Blackboard));

	// Test design:
	// - Behavior under test: each frozen canonical WRITE caller performs exactly
	//   one balanced DefaultValue lifecycle, while pre-live rejection stays inert
	//   and an after-assignment rejection still dispatches its matching post-edit.
	// - Candidate technique: lifecycle state transition with failure injection.
	// - Risk or gap: the native assignment can bypass observation, omit post-edit,
	//   or write after a failed prepared identity recheck.
	// - Test: three successful callers, then identity and injected failure cells.
	// - Expected observable outcome: one exact event per success; zero before-live
	//   events; and an after-assignment event that has both lifecycle halves.
	const FAssetDocumentRuntimeValueDto LifecycleDesired = ObjectValue({TEXT("DefaultValue")},
		{ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue({TEXT("Count"), TEXT("Label")},
				{IntegerValue(73), StringValue(TEXT("lifecycle"))}),
				StringValue(FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct()->GetPathName())})});
	const auto MakeLifecycleTarget = [&]()
	{
		UBlackboardKeyType_Struct* const Result = NewObject<UBlackboardKeyType_Struct>(
			Blackboard, NAME_None, RF_Transactional);
		Result->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
		Result->DefaultValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Count = 7;
		return Result;
	};
	const auto PrepareAndApplyLifecycle = [&](const ECaller Caller, UBlackboardKeyType_Struct& Target,
		FBlackboardLifecycleTransaction& CaseTransaction)
	{
		const FPrepareResult PreparedLifecycle = PrepareCanonicalWrite(Caller, *Blackboard,
			*UBlackboardKeyType_Struct::StaticClass(),
			Caller == ECaller::UpdateExistingV2 ? &Target : nullptr, CaseTransaction, LifecycleDesired);
		return PreparedLifecycle.bSucceeded
			? ApplyPreparedCanonicalWrite(PreparedLifecycle.Prepared, Caller, Target, CaseTransaction)
			: FApplyResult{false, PreparedLifecycle.Failure};
	};

	// SC-WR-003 BalancedLifecycleAndRollback: caller identity is a part of the
	// audit contract, not an implementation detail of any row owner.
	ResetLifecycleAudit();
	TArray<ECaller> SuccessfulCallers;
	for (const ECaller Caller : {ECaller::UpdateExistingV2, ECaller::AddV2, ECaller::ReplaceClassV2})
	{
		UBlackboardKeyType_Struct* const Target = MakeLifecycleTarget();
		FBlackboardLifecycleTransaction CaseTransaction;
		const FApplyResult AppliedLifecycle = PrepareAndApplyLifecycle(Caller, *Target, CaseTransaction);
		Passed &= TestTrue(*FString::Printf(TEXT("SC-WR-003 %d caller applies canonical lifecycle"),
			static_cast<int32>(Caller)), AppliedLifecycle.bSucceeded);
		SuccessfulCallers.Add(Caller);
	}
	const TArray<FLifecycleAuditEvent> SuccessfulLifecycleAudit = GetLifecycleAudit();
	Passed &= TestEqual(TEXT("SC-WR-003 records one balanced lifecycle per exact caller"),
		SuccessfulLifecycleAudit.Num(), SuccessfulCallers.Num());
	for (int32 Index = 0; Index < SuccessfulLifecycleAudit.Num() && SuccessfulCallers.IsValidIndex(Index); ++Index)
	{
		const FLifecycleAuditEvent& Event = SuccessfulLifecycleAudit[Index];
		Passed &= TestTrue(*FString::Printf(TEXT("SC-WR-003 event %d has exact caller and DefaultValue lifecycle"), Index),
			Event.Caller == SuccessfulCallers[Index] && Event.bDefaultValueProperty
				&& Event.bDefaultValueMember && Event.bEnteredPreEdit && Event.bPostEditDispatched);
	}

	UBlackboardKeyType_Struct* const IdentityTarget = MakeLifecycleTarget();
	UBlackboardKeyType_Struct* const DifferentTarget = MakeLifecycleTarget();
	FBlackboardLifecycleTransaction IdentityTransaction;
	const FPrepareResult IdentityPrepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2,
		*Blackboard, *UBlackboardKeyType_Struct::StaticClass(), IdentityTarget, IdentityTransaction, LifecycleDesired);
	ResetLifecycleAudit();
	const FApplyResult IdentityRejected = IdentityPrepared.bSucceeded
		? ApplyPreparedCanonicalWrite(IdentityPrepared.Prepared, ECaller::UpdateExistingV2,
			*DifferentTarget, IdentityTransaction) : FApplyResult{false, IdentityPrepared.Failure};
	Passed &= TestFalse(TEXT("SC-WR-003 prepared identity recheck rejects a different live target"),
		IdentityRejected.bSucceeded);
	Passed &= TestTrue(TEXT("SC-WR-003 prepared identity recheck emits zero live lifecycle events"),
		GetLifecycleAudit().IsEmpty());

	UBlackboardKeyType_Struct* const BeforeLiveTarget = MakeLifecycleTarget();
	FBlackboardLifecycleTransaction BeforeLiveTransaction;
	ResetLifecycleAudit();
	FApplyResult BeforeLiveResult;
	{
		FScopedLifecycleTestControl Control({ELifecycleTestFailurePoint::BeforeLiveLifecycle});
		BeforeLiveResult = PrepareAndApplyLifecycle(ECaller::UpdateExistingV2, *BeforeLiveTarget, BeforeLiveTransaction);
	}
	Passed &= TestFalse(TEXT("SC-WR-003 before-live injection failure-closes"), BeforeLiveResult.bSucceeded);
	Passed &= TestTrue(TEXT("SC-WR-003 before-live injection emits zero lifecycle events"),
		GetLifecycleAudit().IsEmpty());

	const UBlackboardKeyType_Struct* const OldAfterAssignmentTarget = KeyType;
	const FAssetDocumentAIBlackboardCanonicalStructNested* const OldAfterAssignmentValue =
		KeyType->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>();
	const int32 OldAfterAssignmentCount = OldAfterAssignmentValue == nullptr ? INDEX_NONE : OldAfterAssignmentValue->Count;
	const FString OldAfterAssignmentLabel = OldAfterAssignmentValue == nullptr ? FString() : OldAfterAssignmentValue->Label;
	const UScriptStruct* const OldAfterAssignmentSchema = KeyType->DefaultValue.GetScriptStruct();
	TArray<FName> OldAfterAssignmentOrder;
	for (const FBlackboardEntry& ExistingEntry : Blackboard->Keys) OldAfterAssignmentOrder.Add(ExistingEntry.EntryName);
	const bool bOldAfterAssignmentDirty = Package->IsDirty();
	FBlackboardLifecycleTransaction AfterAssignmentTransaction;
	FBlackboardLifecycleHandleResolver AfterAssignmentHandles(AfterAssignmentTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> AfterAssignmentEditor = ResolveEditorMutableAsset(
		*this, *Blackboard, AfterAssignmentTransaction.GetMutationToken(), AfterAssignmentHandles);
	if (!TestTrue(TEXT("SC-WR-003 after-assignment fixture obtains an editor asset handle"),
		AfterAssignmentEditor.IsSet())) return false;
	const TOptional<FAssetDocumentOpaqueHandle> AfterAssignmentHandle =
		AfterAssignmentHandles.AddProviderLocalAIResult(AfterAssignmentEditor.GetValue());
	if (!TestTrue(TEXT("SC-WR-003 after-assignment fixture obtains an AI asset handle"),
		AfterAssignmentHandle.IsSet())) return false;
	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest AfterAssignmentRequest;
	AfterAssignmentRequest.BlackboardPath = Blackboard->GetPathName();
	AfterAssignmentRequest.KeyName = TEXT("StructTarget");
	AfterAssignmentRequest.ExpectedClassPath = UBlackboardKeyType_Struct::StaticClass()->GetPathName();
	AfterAssignmentRequest.CanonicalProperties = LifecycleDesired;
	AfterAssignmentRequest.Description = TEXT("");
	AfterAssignmentRequest.Category = TEXT("");
	AfterAssignmentRequest.bInstanceSynced = false;
	ResetLifecycleAudit();
	FAssetDocumentPrimitiveOutcome AfterAssignmentResult;
	{
		FScopedLifecycleTestControl Control({ELifecycleTestFailurePoint::AfterAssignment});
		AfterAssignmentResult = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
			MakeUpdateExistingInvocation(AfterAssignmentHandle.GetValue(), AfterAssignmentRequest),
			AfterAssignmentHandles, &AfterAssignmentTransaction);
	}
	const TArray<FLifecycleAuditEvent> AfterAssignmentAudit = GetLifecycleAudit();
	Passed &= TestFalse(TEXT("SC-WR-003 after-assignment injection failure-closes"),
		AfterAssignmentResult.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestTrue(TEXT("SC-WR-003 after-assignment injection preserves the existing late-write terminal"),
		HasDiagnostic(AfterAssignmentResult, TEXT("AIProvider.Blackboard.DerivedRefreshFailed")));
	Passed &= TestEqual(TEXT("SC-WR-003 after-assignment injection records its entered lifecycle once"),
		AfterAssignmentAudit.Num(), 1);
	if (AfterAssignmentAudit.Num() == 1)
	{
		const FLifecycleAuditEvent& Event = AfterAssignmentAudit[0];
		Passed &= TestTrue(TEXT("SC-WR-003 after-assignment injection dispatches matching DefaultValue post-edit"),
			Event.Caller == ECaller::UpdateExistingV2 && Event.bDefaultValueProperty
				&& Event.bDefaultValueMember && Event.bEnteredPreEdit && Event.bPostEditDispatched);
	}
	const FAssetDocumentContractValidationResult AfterAssignmentRollback =
		AfterAssignmentTransaction.Rollback(TEXT("Test.SC-WR-003.AfterAssignment"));
	TArray<FName> RestoredAfterAssignmentOrder;
	for (const FBlackboardEntry& RestoredEntry : Blackboard->Keys) RestoredAfterAssignmentOrder.Add(RestoredEntry.EntryName);
	const int32 RestoredAfterAssignmentIndex = Blackboard->Keys.IndexOfByPredicate([](const FBlackboardEntry& Entry)
	{
		return Entry.EntryName == FName(TEXT("StructTarget"));
	});
	const UBlackboardKeyType_Struct* const RestoredAfterAssignmentTarget =
		Blackboard->Keys.IsValidIndex(RestoredAfterAssignmentIndex)
			? Cast<UBlackboardKeyType_Struct>(Blackboard->Keys[RestoredAfterAssignmentIndex].KeyType) : nullptr;
	const FAssetDocumentAIBlackboardCanonicalStructNested* const RestoredAfterAssignmentValue =
		RestoredAfterAssignmentTarget == nullptr ? nullptr
			: RestoredAfterAssignmentTarget->DefaultValue.GetPtr<FAssetDocumentAIBlackboardCanonicalStructNested>();
	const UScriptStruct* const RestoredAfterAssignmentSchema = RestoredAfterAssignmentTarget == nullptr
		? nullptr : RestoredAfterAssignmentTarget->DefaultValue.GetScriptStruct();
	const int32 RestoredAfterAssignmentCount = RestoredAfterAssignmentValue == nullptr
		? INDEX_NONE : RestoredAfterAssignmentValue->Count;
	const FString RestoredAfterAssignmentLabel = RestoredAfterAssignmentValue == nullptr
		? TEXT("<null>") : RestoredAfterAssignmentValue->Label;
	const bool bRollbackValid = AfterAssignmentRollback.bValid;
	const bool bTargetRestored = RestoredAfterAssignmentTarget == OldAfterAssignmentTarget;
	const bool bValueRestored = RestoredAfterAssignmentValue != nullptr;
	const bool bSchemaRestored = RestoredAfterAssignmentSchema == OldAfterAssignmentSchema;
	const bool bCountRestored = RestoredAfterAssignmentCount == OldAfterAssignmentCount;
	const bool bLabelRestored = RestoredAfterAssignmentLabel == OldAfterAssignmentLabel;
	const bool bOrderRestored = RestoredAfterAssignmentOrder == OldAfterAssignmentOrder;
	const bool bDirtyRestored = Package->IsDirty() == bOldAfterAssignmentDirty;
	const bool bExactRollbackRestored = bRollbackValid && bTargetRestored && bValueRestored
		&& bSchemaRestored && bCountRestored && bLabelRestored && bOrderRestored && bDirtyRestored;
	if (!bExactRollbackRestored)
	{
		AddInfo(FString::Printf(TEXT("SC-WR-003 rollback mismatch: valid=%d target=%d actual=%p expected=%p value=%d actual=%p schema=%d actual=%p expected=%p count=%d/%d label='%s'/'%s' order=%d actual=[%s] expected=[%s] dirty=%d/%d"),
			bRollbackValid, bTargetRestored, static_cast<const void*>(RestoredAfterAssignmentTarget),
			static_cast<const void*>(OldAfterAssignmentTarget), bValueRestored,
			static_cast<const void*>(RestoredAfterAssignmentValue), bSchemaRestored,
			static_cast<const void*>(RestoredAfterAssignmentSchema), static_cast<const void*>(OldAfterAssignmentSchema),
			RestoredAfterAssignmentCount, OldAfterAssignmentCount, *RestoredAfterAssignmentLabel,
			*OldAfterAssignmentLabel, bOrderRestored,
			*FString::JoinBy(RestoredAfterAssignmentOrder, TEXT(","), [](FName Name) { return Name.ToString(); }),
			*FString::JoinBy(OldAfterAssignmentOrder, TEXT(","), [](FName Name) { return Name.ToString(); }),
			Package->IsDirty(), bOldAfterAssignmentDirty));
	}
	Passed &= TestTrue(TEXT("SC-WR-003 Atomic rollback restores exact Struct pointer/value/order/dirty state"),
		bExactRollbackRestored);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardKeyTypeStructCanonicalReadTest,
	"UEAssetDoc.AIProvider.BlackboardKeyTypeStructCanonicalRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardKeyTypeStructCanonicalReadTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	using namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalRead;
	// Test design:
	// - Behavior: the private no-load codec emits the frozen exact DTO for one
	//   representative containing every admitted scalar/reference/container kind.
	// - Technique: equivalence partition with literal expected output; ordering
	//   and closed-terminal partitions are covered by separate tests.
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/StructCanonical_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlackboardData* HardObject = NewObject<UBlackboardData>(
		Package, TEXT("HardObject"), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("AdmittedAlgebra hard reference fixture exists"), HardObject))
	{
		return false;
	}
	FInstancedStruct Source;
	Source.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	FAssetDocumentAIBlackboardCanonicalStructAdmitted& Value =
		Source.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	Value.ArrayValues = {9, -2};
	Value.BoolValue = true;
	Value.ByteValue = 255;
	Value.ClassValue = UObject::StaticClass();
	Value.DoubleValue = 12.5;
	Value.DynamicValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Value.DynamicValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Count = 4;
	Value.DynamicValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Label = TEXT("dynamic");
	Value.EnumValue = EAssetDocumentAIBlackboardCanonicalStructTestEnum::Second;
	Value.FloatValue = 1.25f;
	Value.HardObject = HardObject;
	Value.Int64Value = MIN_int64 + 7;
	Value.IntValue = -17;
	Value.MapValues.Add(TEXT("b"), 2);
	Value.MapValues.Add(TEXT("a"), 1);
	Value.NameValue = FName(TEXT("CanonicalName_7"));
	Value.NestedValue.Count = 3;
	Value.NestedValue.Label = TEXT("nested");
	Value.SetValues.Add(TEXT("zulu"));
	Value.SetValues.Add(TEXT("alpha"));
	Value.SoftClassValue = TSoftClassPtr<UObject>(UObject::StaticClass());
	Value.SoftObjectValue = TSoftObjectPtr<UObject>(HardObject);
	Value.StaticValues[0] = 5;
	Value.StaticValues[1] = 6;
	Value.StaticValues[2] = 7;
	Value.StringValue = TEXT("canonical");
	Value.TextValue = FText::AsCultureInvariant(TEXT("literal-equivalent"));
	Value.UInt16Value = MAX_uint16;
	Value.UInt32Value = MAX_uint32;
	Value.UInt64Value = MAX_uint64;

	const auto StructValue = [](const UScriptStruct& Struct, TArray<FString> Keys,
		TArray<FAssetDocumentRuntimeValueDto> Values)
	{
		return ObjectValue({TEXT("Properties"), TEXT("Struct")},
			{ObjectValue(MoveTemp(Keys), MoveTemp(Values)), StringValue(Struct.GetPathName())});
	};
	const FAssetDocumentRuntimeValueDto Nested = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(3), StringValue(TEXT("nested"))});
	const FAssetDocumentRuntimeValueDto Dynamic = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructNested::StaticStruct(),
		{TEXT("Count"), TEXT("Label")}, {IntegerValue(4), StringValue(TEXT("dynamic"))});
	const FAssetDocumentRuntimeValueDto Expected = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructAdmitted::StaticStruct(),
		{TEXT("ArrayValues"), TEXT("BoolValue"), TEXT("ByteValue"), TEXT("ClassValue"),
			TEXT("DoubleValue"), TEXT("DynamicValue"), TEXT("EnumValue"), TEXT("FloatValue"),
			TEXT("HardObject"), TEXT("Int64Value"), TEXT("IntValue"), TEXT("MapValues"),
			TEXT("NameValue"), TEXT("NestedValue"), TEXT("SetValues"), TEXT("SoftClassValue"),
			TEXT("SoftObjectValue"), TEXT("StaticValues"), TEXT("StringValue"), TEXT("TextValue"),
			TEXT("UInt16Value"), TEXT("UInt32Value"), TEXT("UInt64Value")},
		{ArrayValue({IntegerValue(9), IntegerValue(-2)}), BooleanValue(true), IntegerValue(255),
			ReferenceValue(TEXT("ClassRef"), *UObject::StaticClass()), NumberValue(12.5), Dynamic,
			ObjectValue({TEXT("Enum"), TEXT("Enumerator")},
				{StringValue(StaticEnum<EAssetDocumentAIBlackboardCanonicalStructTestEnum>()->GetPathName()),
					StringValue(TEXT("Second"))}),
			NumberValue(1.25), ReferenceValue(TEXT("AssetRef"), *HardObject),
			IntegerValue(MIN_int64 + 7), IntegerValue(-17),
			ArrayValue({ObjectValue({TEXT("Key"), TEXT("Value")},
				{StringValue(TEXT("a")), IntegerValue(1)}),
				ObjectValue({TEXT("Key"), TEXT("Value")},
				{StringValue(TEXT("b")), IntegerValue(2)})}),
			StringValue(TEXT("CanonicalName_7")), Nested,
			ArrayValue({StringValue(TEXT("zulu")), StringValue(TEXT("alpha"))}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftClassRef")), StringValue(UObject::StaticClass()->GetPathName())}),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("SoftAssetRef")), StringValue(HardObject->GetPathName())}),
			ArrayValue({IntegerValue(5), IntegerValue(6), IntegerValue(7)}),
			StringValue(TEXT("canonical")), StringValue(TEXT("literal-equivalent")),
			IntegerValue(MAX_uint16), IntegerValue(MAX_uint32),
			ObjectValue({TEXT("Kind"), TEXT("Value")},
				{StringValue(TEXT("UInt64")), StringValue(TEXT("18446744073709551615"))})});

	const FReadResult Read = ReadCanonicalValue(Source, ESource::DetachedScratch, nullptr);
	bool Passed = TestTrue(*FString::Printf(TEXT("AdmittedAlgebra succeeds; internal failure=%d"),
		static_cast<int32>(Read.Failure)), Read.bSucceeded);
	const FString Difference = FirstRuntimeValueDifference(Read.Value, Expected);
	Passed &= TestTrue(*FString::Printf(
		TEXT("AdmittedAlgebra emits the exact complete canonical DTO; first difference: %s"),
		Difference.IsEmpty() ? TEXT("none") : *Difference), Difference.IsEmpty());

	FInstancedStruct Invalid;
	const FReadResult InvalidRead = ReadCanonicalValue(Invalid, ESource::DetachedScratch, nullptr);
	Passed &= TestTrue(TEXT("AdmittedAlgebra invalid top-level value succeeds as canonical Null"),
		InvalidRead.bSucceeded && RuntimeValueEquals(InvalidRead.Value, NullValue()));

	FInstancedStruct Inherited;
	Inherited.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructDerived>();
	auto& InheritedValue = Inherited.GetMutable<FAssetDocumentAIBlackboardCanonicalStructDerived>();
	InheritedValue.AlphaDerived = 1;
	InheritedValue.ZuluBase = 2;
	const FReadResult InheritedRead = ReadCanonicalValue(Inherited, ESource::DetachedScratch, nullptr);
	const FAssetDocumentRuntimeValueDto ExpectedInherited = StructValue(
		*FAssetDocumentAIBlackboardCanonicalStructDerived::StaticStruct(),
		{TEXT("AlphaDerived"), TEXT("ZuluBase")}, {IntegerValue(1), IntegerValue(2)});
	Passed &= TestTrue(TEXT("AdmittedAlgebra includes inherited stored fields in lexical order"),
		InheritedRead.bSucceeded && RuntimeValueEquals(InheritedRead.Value, ExpectedInherited));

	FInstancedStruct OrderA;
	FInstancedStruct OrderB;
	OrderA.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	OrderB.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	auto& A = OrderA.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	auto& B = OrderB.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	A.SetValues.Add(TEXT("one")); A.SetValues.Add(TEXT("three")); A.SetValues.Add(TEXT("two"));
	B.SetValues.Add(TEXT("two")); B.SetValues.Add(TEXT("one")); B.SetValues.Add(TEXT("three"));
	A.MapValues.Add(TEXT("z"), 26); A.MapValues.Add(TEXT("a"), 1); A.MapValues.Add(TEXT("m"), 13);
	B.MapValues.Add(TEXT("a"), 1); B.MapValues.Add(TEXT("m"), 13); B.MapValues.Add(TEXT("z"), 26);
	const FReadResult ReadA = ReadCanonicalValue(OrderA, ESource::DetachedScratch, nullptr);
	const FReadResult ReadB = ReadCanonicalValue(OrderB, ESource::DetachedScratch, nullptr);
	Passed &= TestTrue(TEXT("CanonicalOrdering succeeds for both insertion orders"),
		ReadA.bSucceeded && ReadB.bSucceeded);
	Passed &= TestTrue(TEXT("CanonicalOrdering is byte-for-byte DTO equivalent"),
		RuntimeValueEquals(ReadA.Value, ReadB.Value));
	FRandomStream InsertionOrder(0x5C2D001);
	for (int32 Trial = 0; Trial < 24; ++Trial)
	{
		TArray<FString> SetInsertion{TEXT("one"), TEXT("three"), TEXT("two")};
		TArray<TPair<FString, int32>> MapInsertion{
			{TEXT("z"), 26}, {TEXT("a"), 1}, {TEXT("m"), 13}};
		for (int32 Index = SetInsertion.Num() - 1; Index > 0; --Index)
			SetInsertion.Swap(Index, InsertionOrder.RandRange(0, Index));
		for (int32 Index = MapInsertion.Num() - 1; Index > 0; --Index)
			MapInsertion.Swap(Index, InsertionOrder.RandRange(0, Index));
		FInstancedStruct Permuted;
		Permuted.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
		auto& PermutedValue = Permuted.GetMutable<
			FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
		for (const FString& Element : SetInsertion) PermutedValue.SetValues.Add(Element);
		for (const TPair<FString, int32>& Pair : MapInsertion)
			PermutedValue.MapValues.Add(Pair.Key, Pair.Value);
		const FReadResult PermutedRead = ReadCanonicalValue(
			Permuted, ESource::DetachedScratch, nullptr);
		Passed &= TestTrue(*FString::Printf(TEXT("CanonicalOrdering seeded trial %d succeeds"), Trial),
			PermutedRead.bSucceeded && RuntimeValueEquals(PermutedRead.Value, ReadA.Value));
	}
	const FAssetDocumentRuntimeValueDto* DefaultProperties =
		FindRuntimeValueField(ReadA.Value, TEXT("Properties"));
	const FAssetDocumentRuntimeValueDto* DefaultName = DefaultProperties != nullptr
		? FindRuntimeValueField(*DefaultProperties, TEXT("NameValue")) : nullptr;
	Passed &= TestTrue(TEXT("AdmittedAlgebra NAME_None has an exact canonical field"),
		DefaultName != nullptr && DefaultName->Kind == EAssetDocumentRuntimeValueKind::String);
	if (DefaultName != nullptr)
	{
		Passed &= TestEqual(TEXT("AdmittedAlgebra NAME_None is the empty string"),
			DefaultName->StringValue, FString());
	}
	const FAssetDocumentRuntimeValueDto* DefaultText = DefaultProperties != nullptr
		? FindRuntimeValueField(*DefaultProperties, TEXT("TextValue")) : nullptr;
	Passed &= TestTrue(TEXT("AdmittedAlgebra empty FText is an exact empty string"),
		DefaultText != nullptr && DefaultText->Kind == EAssetDocumentRuntimeValueKind::String
			&& DefaultText->StringValue.IsEmpty());

	const auto RequireClosed = [this, &Passed](const TCHAR* Label, const FReadResult& Failure,
		const EFailure ExpectedFailure)
	{
		Passed &= TestFalse(*FString::Printf(TEXT("%s fails closed"), Label), Failure.bSucceeded);
		Passed &= TestEqual(*FString::Printf(TEXT("%s has its exact internal terminal"), Label),
			Failure.Failure, ExpectedFailure);
		Passed &= TestTrue(*FString::Printf(TEXT("%s publishes no partial DTO"), Label),
			RuntimeValueEquals(Failure.Value, NullValue()));
	};
	RequireClosed(TEXT("ClosedTerminals unknown source authority"),
		ReadCanonicalValue(Source, static_cast<ESource>(0xff), nullptr), EFailure::InvalidContext);
	UUserDefinedStruct* AuthoredStruct = FStructureEditorUtils::CreateUserDefinedStruct(
		Package, TEXT("CanonicalAuthoredNameStruct"), RF_Public);
	FProperty* AuthoredProperty = FStructureEditorUtils::GetPropertyByFriendlyName(
		AuthoredStruct, TEXT("MemberVar_0"));
	if (!TestNotNull(TEXT("CanonicalOrdering creates a valid UDS property"), AuthoredProperty))
	{
		return false;
	}
	const FGuid AuthoredPropertyGuid = FStructureEditorUtils::GetGuidForProperty(AuthoredProperty);
	if (!TestTrue(TEXT("CanonicalOrdering finds the UDS property GUID"),
			AuthoredPropertyGuid.IsValid())
		|| !TestTrue(TEXT("CanonicalOrdering assigns the UDS authored name"),
			FStructureEditorUtils::RenameVariable(
				AuthoredStruct, AuthoredPropertyGuid, TEXT("AuthoredField"))))
	{
		return false;
	}
	FStructureEditorUtils::CompileStructure(AuthoredStruct);
	AuthoredProperty = FStructureEditorUtils::GetPropertyByFriendlyName(
		AuthoredStruct, TEXT("AuthoredField"));
	if (!TestNotNull(TEXT("CanonicalOrdering retains the compiled UDS property"), AuthoredProperty))
	{
		return false;
	}
	FInstancedStruct AuthoredValue;
	AuthoredValue.InitializeAs(AuthoredStruct);
	CastFieldChecked<FBoolProperty>(AuthoredProperty)->SetPropertyValue_InContainer(
		AuthoredValue.GetMutableMemory(), true);
	const FReadResult AuthoredRead = ReadCanonicalValue(
		AuthoredValue, ESource::DetachedScratch, nullptr);
	const FAssetDocumentRuntimeValueDto ExpectedAuthored = StructValue(*AuthoredStruct,
		{TEXT("AuthoredField")}, {BooleanValue(true)});
	Passed &= TestTrue(TEXT("CanonicalOrdering uses UDS authored names instead of internal GUID names"),
		AuthoredProperty->GetName() != AuthoredProperty->GetAuthoredName()
			&& AuthoredRead.bSucceeded && RuntimeValueEquals(AuthoredRead.Value, ExpectedAuthored));
	FInstancedStruct Unsupported;
	Unsupported.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructUnsupported>();
	RequireClosed(TEXT("ClosedTerminals unsupported weak reference"),
		ReadCanonicalValue(Unsupported, ESource::DetachedScratch, nullptr), EFailure::UnsupportedProperty);

	FInstancedStruct TransientProperty;
	TransientProperty.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructTransient>();
	RequireClosed(TEXT("ClosedTerminals transient property flag"),
		ReadCanonicalValue(TransientProperty, ESource::DetachedScratch, nullptr),
		EFailure::UnsupportedProperty);

	FInstancedStruct Collision;
	Collision.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructCollisionSet>();
	auto& CollisionValues =
		Collision.GetMutable<FAssetDocumentAIBlackboardCanonicalStructCollisionSet>().Values;
	FAssetDocumentAIBlackboardCanonicalStructCollisionKey PositiveZero;
	PositiveZero.Value = 0.0f;
	FAssetDocumentAIBlackboardCanonicalStructCollisionKey NegativeZero;
	NegativeZero.Value = -0.0f;
	CollisionValues.Add(PositiveZero);
	CollisionValues.Add(NegativeZero);
	RequireClosed(TEXT("CanonicalOrdering rejects canonical set collisions"),
		ReadCanonicalValue(Collision, ESource::DetachedScratch, nullptr),
		EFailure::CanonicalCollision);
	FInstancedStruct MapCollision;
	MapCollision.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructCollisionMap>();
	auto& MapCollisionValues =
		MapCollision.GetMutable<FAssetDocumentAIBlackboardCanonicalStructCollisionMap>().Values;
	MapCollisionValues.Add(PositiveZero, 1);
	MapCollisionValues.Add(NegativeZero, 2);
	RequireClosed(TEXT("CanonicalOrdering rejects canonical map-key collisions"),
		ReadCanonicalValue(MapCollision, ESource::DetachedScratch, nullptr),
		EFailure::CanonicalCollision);

	FInstancedStruct NonFinite;
	NonFinite.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	NonFinite.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().DoubleValue =
		std::numeric_limits<double>::infinity();
	RequireClosed(TEXT("ClosedTerminals non-finite number"),
		ReadCanonicalValue(NonFinite, ESource::DetachedScratch, nullptr), EFailure::NonFiniteNumber);
	NonFinite.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().DoubleValue =
		std::numeric_limits<double>::quiet_NaN();
	RequireClosed(TEXT("ClosedTerminals NaN number"),
		ReadCanonicalValue(NonFinite, ESource::DetachedScratch, nullptr), EFailure::NonFiniteNumber);

	FInstancedStruct InvalidEnum;
	InvalidEnum.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	InvalidEnum.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().EnumValue =
		EAssetDocumentAIBlackboardCanonicalStructTestEnum::HiddenValue;
	RequireClosed(TEXT("ClosedTerminals hidden enum"),
		ReadCanonicalValue(InvalidEnum, ESource::DetachedScratch, nullptr), EFailure::InvalidEnum);

	FInstancedStruct UnstableText;
	UnstableText.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	UnstableText.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().TextValue =
		FText::FromString(TEXT("localizable"));
	RequireClosed(TEXT("ClosedTerminals non-invariant text"),
		ReadCanonicalValue(UnstableText, ESource::DetachedScratch, nullptr), EFailure::UnstableText);
	UnstableText.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().TextValue =
		FText::AsNumber(42);
	RequireClosed(TEXT("ClosedTerminals historic numeric text"),
		ReadCanonicalValue(UnstableText, ESource::DetachedScratch, nullptr), EFailure::UnstableText);
	UnstableText.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().TextValue =
		FText::Format(FText::AsCultureInvariant(TEXT("{0}")),
			FText::AsCultureInvariant(TEXT("formatted")));
	RequireClosed(TEXT("ClosedTerminals historic format text"),
		ReadCanonicalValue(UnstableText, ESource::DetachedScratch, nullptr), EFailure::UnstableText);
	UnstableText.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().TextValue =
		FText::ChangeKey(TEXT("AssetDocumentAI"), TEXT("CanonicalRead"),
			FText::FromString(TEXT("localized")));
	RequireClosed(TEXT("ClosedTerminals keyed localized text"),
		ReadCanonicalValue(UnstableText, ESource::DetachedScratch, nullptr), EFailure::UnstableText);
	AddExpectedError(TEXT("Failed to find string table entry for"),
		EAutomationExpectedErrorFlags::Contains, 1);
	UnstableText.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().TextValue =
		FText::FromStringTable(TEXT("/Game/AssetDocumentAI/MissingStringTable"),
			TEXT("MissingKey"), EStringTableLoadingPolicy::Find);
	RequireClosed(TEXT("ClosedTerminals string-table text"),
		ReadCanonicalValue(UnstableText, ESource::DetachedScratch, nullptr), EFailure::UnstableText);

	FInstancedStruct TransientReference;
	TransientReference.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	TransientReference.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().HardObject =
		NewObject<UBlackboardData>();
	RequireClosed(TEXT("ClosedTerminals transient hard reference"),
		ReadCanonicalValue(TransientReference, ESource::DetachedScratch, nullptr),
		EFailure::UnstableReference);

	const auto MakeDepthValue = [](const int32 NestedCount)
	{
		FInstancedStruct Result;
		Result.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructDepth>();
		FAssetDocumentAIBlackboardCanonicalStructDepth* Current =
			&Result.GetMutable<FAssetDocumentAIBlackboardCanonicalStructDepth>();
		for (int32 Index = 0; Index < NestedCount; ++Index)
		{
			Current->Next.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructDepth>();
			Current = &Current->Next.GetMutable<FAssetDocumentAIBlackboardCanonicalStructDepth>();
		}
		return Result;
	};
	const FReadResult DepthBelow = ReadCanonicalValue(
		MakeDepthValue(14), ESource::DetachedScratch, nullptr);
	const FReadResult DepthExact = ReadCanonicalValue(
		MakeDepthValue(15), ESource::DetachedScratch, nullptr);
	Passed &= TestTrue(*FString::Printf(
		TEXT("ClosedTerminals depth below and exact limits succeed; failures=%d/%d"),
		static_cast<int32>(DepthBelow.Failure), static_cast<int32>(DepthExact.Failure)),
		DepthBelow.bSucceeded && DepthExact.bSucceeded);
	RequireClosed(TEXT("ClosedTerminals depth one-over limit"),
		ReadCanonicalValue(MakeDepthValue(16), ESource::DetachedScratch, nullptr),
		EFailure::BudgetExceeded);

	const auto MakeArrayBudgetValue = [](const int32 Count)
	{
		FInstancedStruct Result;
		Result.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructArrayOnly>();
		Result.GetMutable<FAssetDocumentAIBlackboardCanonicalStructArrayOnly>().Values.Init(7, Count);
		return Result;
	};
	const FReadResult ContainerBelow = ReadCanonicalValue(
		MakeArrayBudgetValue(1023), ESource::DetachedScratch, nullptr);
	const FReadResult ContainerExact = ReadCanonicalValue(
		MakeArrayBudgetValue(1024), ESource::DetachedScratch, nullptr);
	Passed &= TestTrue(*FString::Printf(
		TEXT("ClosedTerminals container below and exact limits succeed; failures=%d/%d"),
		static_cast<int32>(ContainerBelow.Failure), static_cast<int32>(ContainerExact.Failure)),
		ContainerBelow.bSucceeded && ContainerExact.bSucceeded);
	RequireClosed(TEXT("ClosedTerminals container one-over limit"),
		ReadCanonicalValue(MakeArrayBudgetValue(1025), ESource::DetachedScratch, nullptr),
		EFailure::BudgetExceeded);

	const FInstancedStruct EmptyArrayValue = MakeArrayBudgetValue(0);
	const auto ReadWithControl = [&EmptyArrayValue](const FReadTestControl& Control)
	{
		AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::Private::FScopedReadTestControl Scope(Control);
		return ReadCanonicalValue(EmptyArrayValue, ESource::DetachedScratch, nullptr);
	};
	FReadTestControl FieldBelow;
	FieldBelow.PrechargeSchemaFields = 254;
	FieldBelow.PrechargeFields = 254;
	FReadTestControl FieldExact;
	FieldExact.PrechargeSchemaFields = 255;
	FieldExact.PrechargeFields = 255;
	FReadTestControl FieldOver;
	FieldOver.PrechargeSchemaFields = 256;
	FieldOver.PrechargeFields = 256;
	Passed &= TestTrue(TEXT("ClosedTerminals field below and exact limits succeed"),
		ReadWithControl(FieldBelow).bSucceeded && ReadWithControl(FieldExact).bSucceeded);
	RequireClosed(TEXT("ClosedTerminals field one-over limit"), ReadWithControl(FieldOver),
		EFailure::BudgetExceeded);

	FReadTestControl NodesBelow;
	NodesBelow.PrechargeNodes = 4091;
	FReadTestControl NodesExact;
	NodesExact.PrechargeNodes = 4092;
	FReadTestControl NodesOver;
	NodesOver.PrechargeNodes = 4093;
	Passed &= TestTrue(TEXT("ClosedTerminals node below and exact limits succeed"),
		ReadWithControl(NodesBelow).bSucceeded && ReadWithControl(NodesExact).bSucceeded);
	RequireClosed(TEXT("ClosedTerminals node one-over limit"), ReadWithControl(NodesOver),
		EFailure::BudgetExceeded);
	const FInstancedStruct NullNodeValue = MakeDepthValue(0);
	const auto ReadNullWithControl = [&NullNodeValue](const int32 Precharge)
	{
		FReadTestControl Control;
		Control.PrechargeNodes = Precharge;
		AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::Private::FScopedReadTestControl Scope(Control);
		return ReadCanonicalValue(NullNodeValue, ESource::DetachedScratch, nullptr);
	};
	Passed &= TestTrue(TEXT("ClosedTerminals nested Null succeeds at 4095 and 4096 exact nodes"),
		ReadNullWithControl(4091).bSucceeded && ReadNullWithControl(4092).bSucceeded);
	RequireClosed(TEXT("ClosedTerminals nested Null rejects 4097 nodes"),
		ReadNullWithControl(4093), EFailure::BudgetExceeded);
	FInstancedStruct UInt64NodeValue;
	UInt64NodeValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructUInt64Only>();
	UInt64NodeValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructUInt64Only>().Value = MAX_uint64;
	const auto ReadUInt64WithControl = [&UInt64NodeValue](const int32 Precharge)
	{
		FReadTestControl Control;
		Control.PrechargeNodes = Precharge;
		AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::Private::FScopedReadTestControl Scope(Control);
		return ReadCanonicalValue(UInt64NodeValue, ESource::DetachedScratch, nullptr);
	};
	Passed &= TestTrue(TEXT("ClosedTerminals uint64 wrapper succeeds at 4095 and 4096 exact nodes"),
		ReadUInt64WithControl(4089).bSucceeded && ReadUInt64WithControl(4090).bSucceeded);
	RequireClosed(TEXT("ClosedTerminals uint64 wrapper rejects 4097 nodes"),
		ReadUInt64WithControl(4091), EFailure::BudgetExceeded);

	const int64 EmptyArrayCanonicalBytes = 85
		+ FAssetDocumentAIBlackboardCanonicalStructArrayOnly::StaticStruct()->GetPathName().Len();
	FReadTestControl ComparisonBelow;
	ComparisonBelow.PrechargeComparisonBytes = 1024 * 1024 - EmptyArrayCanonicalBytes - 1;
	FReadTestControl ComparisonExact;
	ComparisonExact.PrechargeComparisonBytes = 1024 * 1024 - EmptyArrayCanonicalBytes;
	FReadTestControl ComparisonOver;
	ComparisonOver.PrechargeComparisonBytes = 1024 * 1024 - EmptyArrayCanonicalBytes + 1;
	Passed &= TestTrue(TEXT("ClosedTerminals comparison-byte below and exact limits succeed"),
		ReadWithControl(ComparisonBelow).bSucceeded && ReadWithControl(ComparisonExact).bSucceeded);
	RequireClosed(TEXT("ClosedTerminals comparison-byte one-over limit"),
		ReadWithControl(ComparisonOver), EFailure::BudgetExceeded);

	FReadTestControl Drift;
	Drift.bForceSchemaDrift = true;
	RequireClosed(TEXT("ClosedTerminals schema drift hook"), ReadWithControl(Drift), EFailure::SchemaDrift);

	FInstancedStruct Oversize;
	Oversize.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	Oversize.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().StringValue =
		FString::ChrN(256 * 1024 + 1, TEXT('x'));
	RequireClosed(TEXT("ClosedTerminals one-over string budget"),
		ReadCanonicalValue(Oversize, ESource::DetachedScratch, nullptr), EFailure::BudgetExceeded);
	Oversize.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().StringValue =
		FString::ChrN(256 * 1024, TEXT('x'));
	Passed &= TestTrue(TEXT("ClosedTerminals exact string byte limit succeeds"),
		ReadCanonicalValue(Oversize, ESource::DetachedScratch, nullptr).bSucceeded);
	Oversize.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().StringValue =
		FString::ChrN(256 * 1024 - 1, TEXT('x'));
	Passed &= TestTrue(TEXT("ClosedTerminals below string byte limit succeeds"),
		ReadCanonicalValue(Oversize, ESource::DetachedScratch, nullptr).bSucceeded);

	FInstancedStruct NoLoad;
	NoLoad.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	const FSoftObjectPath AbsentPath(TEXT("/Game/AssetDocumentTests/Absent.Absent"));
	NoLoad.GetMutable<FAssetDocumentAIBlackboardCanonicalStructAdmitted>().SoftObjectValue =
		TSoftObjectPtr<UObject>(AbsentPath);
	const bool bAbsentBefore = AbsentPath.ResolveObject() == nullptr;
	const bool bPackageDirtyBefore = Package->IsDirty();
	const FReadResult NoLoadRead = ReadCanonicalValue(NoLoad, ESource::DetachedScratch, nullptr);
	Passed &= TestTrue(TEXT("ClosedTerminals soft path read succeeds without target"),
		bAbsentBefore && NoLoadRead.bSucceeded);
	Passed &= TestNull(TEXT("ClosedTerminals READ does not load the soft target"), AbsentPath.ResolveObject());
	Passed &= TestEqual(TEXT("ClosedTerminals READ does not dirty its source package"),
		Package->IsDirty(), bPackageDirtyBefore);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardKeyTypeStructCanonicalReadFreshReloadTest,
	"UEAssetDoc.AIProvider.BlackboardKeyTypeStructCanonicalFreshReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardKeyTypeStructCanonicalReadFreshReloadTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardLifecycleTests;
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentAIStructRead/BlackboardReadReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	const FString TargetPackageName = FString::Printf(
		TEXT("/Game/AssetDocumentAIStructRead/SoftTarget_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString TargetAssetName = FPackageName::GetLongPackageAssetName(TargetPackageName);
	const FString TargetObjectPath = TargetPackageName + TEXT(".") + TargetAssetName;
	const FString TargetPackageFileName = FPackageName::LongPackageNameToFilename(
		TargetPackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PackageFileName, false, true);
		IFileManager::Get().Delete(*TargetPackageFileName, false, true);
	};

	UPackage* TargetPackage = CreatePackage(*TargetPackageName);
	UBlackboardData* SoftTarget = NewObject<UBlackboardData>(
		TargetPackage, *TargetAssetName, RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("Struct READ real soft target exists"), SoftTarget)) return false;
	FSavePackageArgs TargetSaveArgs;
	TargetSaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	TargetSaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("Struct READ real soft target saves"),
		UPackage::SavePackage(
			TargetPackage, SoftTarget, *TargetPackageFileName, TargetSaveArgs))) return false;
	if (!TestTrue(TEXT("Struct READ real soft target unloads before observation"),
		UPackageTools::UnloadPackages({TargetPackage}))) return false;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (!TestNull(TEXT("Struct READ real soft target starts unloaded"),
		FindPackage(nullptr, *TargetPackageName))) return false;

	UPackage* Package = CreatePackage(*PackageName);
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	UBlackboardKeyType_Struct* KeyType = NewObject<UBlackboardKeyType_Struct>(Blackboard);
	if (!TestNotNull(TEXT("Struct READ fresh-reload Blackboard exists"), Blackboard)
		|| !TestNotNull(TEXT("Struct READ fresh-reload KeyType exists"), KeyType)) return false;
	KeyType->DefaultValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	auto& Stored = KeyType->DefaultValue.GetMutable<
		FAssetDocumentAIBlackboardCanonicalStructAdmitted>();
	Stored.ArrayValues = {4, 9};
	Stored.DynamicValue.InitializeAs<FAssetDocumentAIBlackboardCanonicalStructNested>();
	Stored.DynamicValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Count = 17;
	Stored.DynamicValue.GetMutable<FAssetDocumentAIBlackboardCanonicalStructNested>().Label = TEXT("reload");
	Stored.NestedValue.Count = 11;
	Stored.NestedValue.Label = TEXT("persisted");
	Stored.SoftObjectValue = TSoftObjectPtr<UObject>(FSoftObjectPath(TargetObjectPath));
	Stored.StringValue = TEXT("complete readback");
	Stored.TextValue = FText::AsCultureInvariant(TEXT("literal-equivalent reload"));
	FBlackboardEntry Entry;
	Entry.EntryName = TEXT("PersistedStruct");
	Entry.KeyType = KeyType;
	Blackboard->Keys.Add(Entry);
	Blackboard->UpdateParentKeys();
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateIfHasSynchronizedKeys();
	Package->SetDirtyFlag(false);

	FBlackboardLifecycleTransaction BeforeTransaction;
	FBlackboardLifecycleHandleResolver BeforeHandles(BeforeTransaction.GetMutationToken());
	const FBlackboardObservation BeforeRead(*Blackboard);
	const FAssetDocumentPrimitiveOutcome Before = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryReadbackInvocation(
			*Blackboard, BeforeTransaction.GetMutationToken(), TEXT("PersistedStruct")),
		BeforeHandles, nullptr);
	bool Passed = RequireSuccess(*this, TEXT("Struct READ before persistence"), Before);
	Passed &= TestTrue(TEXT("Struct READ before persistence has no output residue or authored change"),
		Before.HandleResults.IsEmpty() && Before.ChangedSet.IsEmpty()
			&& BeforeTransaction.ActionEnlistCount == 0
			&& BeforeTransaction.EnlistedFootprint.IsEmpty() && BeforeRead.Equals(*Blackboard)
			&& FindPackage(nullptr, *TargetPackageName) == nullptr);
	if (!Passed) return false;

	Package->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	Passed &= TestTrue(TEXT("Struct READ fixture saves through the external lifecycle"),
		UPackage::SavePackage(Package, Blackboard, *PackageFileName, SaveArgs));
	Passed &= TestTrue(TEXT("Struct READ fixture unloads before canonical reload"),
		UPackageTools::UnloadPackages({Package}));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	UBlackboardData* Reloaded = LoadObject<UBlackboardData>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Struct READ Blackboard fresh-reloads"), Reloaded)) return false;
	const FBlackboardObservation ReloadedBefore(*Reloaded);
	const bool bDirtyBefore = Reloaded->GetOutermost()->IsDirty();
	Passed &= TestNull(TEXT("Struct READ real soft dependency remains unloaded before READ"),
		FindPackage(nullptr, *TargetPackageName));

	FBlackboardLifecycleTransaction ReloadedTransaction;
	FBlackboardLifecycleHandleResolver ReloadedHandles(ReloadedTransaction.GetMutationToken());
	const FAssetDocumentPrimitiveOutcome After = CreateAssetDocumentAIPrimitiveProvider()->Invoke(
		MakeEntryReadbackInvocation(
			*Reloaded, ReloadedTransaction.GetMutationToken(), TEXT("PersistedStruct")),
		ReloadedHandles, nullptr);
	Passed &= RequireSuccess(*this, TEXT("Struct READ after fresh reload"), After);
	Passed &= TestTrue(TEXT("Struct READ fresh reload preserves the complete canonical DTO"),
		RuntimeValueEquals(After.Value, Before.Value));
	Passed &= TestTrue(TEXT("Struct READ fresh reload performs no load/action/footprint/dirty mutation"),
		After.HandleResults.IsEmpty() && After.ChangedSet.IsEmpty()
			&& ReloadedTransaction.ActionEnlistCount == 0
			&& ReloadedTransaction.EnlistedFootprint.IsEmpty()
			&& ReloadedBefore.Equals(*Reloaded)
			&& Reloaded->GetOutermost()->IsDirty() == bDirtyBefore
			&& FindPackage(nullptr, *TargetPackageName) == nullptr);
	return Passed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
