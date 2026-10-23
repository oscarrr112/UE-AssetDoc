// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorNativeState.h"

#include "AssetDocumentEditorNativeStateTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetDocumentRollbackAction.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

void UAssetDocumentEditorNativeStateTestObject::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetPropertyName()
		== GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			AuthoredReference))
	{
		DerivedMirror = AuthoredReference;
		++AuthoredReferencePostEditCount;
	}
}

namespace
{
FAssetDocumentContractValidationResult MakeValidContractResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult MakeCaptureContractResult(
	const FAssetDocumentEditorTypedPreimageResult& Capture)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = Capture.bSucceeded;
	Result.Diagnostics = Capture.Diagnostics;
	return Result;
}

bool HasDiagnostic(
	const TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics,
	FStringView Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return true;
		}
	}
	return false;
}

class FNativeStateTestTransaction final
	: public IAssetDocumentAtomicTransaction
{
public:
	explicit FNativeStateTestTransaction(
		const FAssetDocumentEditorProviderTestContext& Context)
	{
		Token.RequestId = Context.RequestId;
		Token.TransactionId = Context.TransactionId;
		Token.RuntimeRevision = Context.RuntimeRevision;
	}

	virtual FGuid GetTransactionId() const override
	{
		return Token.TransactionId;
	}

	virtual EAssetDocumentAtomicState GetState() const override
	{
		return State;
	}

	virtual const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Token;
	}

	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		for (const FAssetDocumentMutationFootprint& Item : Footprint)
		{
			const int32 ActionIndex = Actions.IndexOfByPredicate(
				[&Item](const TSharedRef<IAssetDocumentRollbackAction>& Action)
				{
					return Action->GetRollbackComponentId()
						== Item.RollbackComponentId;
				});
			if (ActionIndex == INDEX_NONE)
			{
				return MakeFailure(TEXT("Test.MissingRollbackAction"));
			}
			if (!RollbackTargetIndices.Contains(ActionIndex))
			{
				RollbackTargetIndices.Add(ActionIndex);
			}
		}
		EnlistedFootprint.Append(Footprint.GetData(), Footprint.Num());
		State = EAssetDocumentAtomicState::Mutating;
		return MakeValidContractResult();
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		State = EAssetDocumentAtomicState::ReadyToCommit;
		return MakeValidContractResult();
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		State = EAssetDocumentAtomicState::Committed;
		Actions.Reset();
		RollbackTargetIndices.Reset();
		return MakeValidContractResult();
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		FAssetDocumentContractValidationResult Result =
			MakeValidContractResult();
		for (int32 TargetIndex = RollbackTargetIndices.Num() - 1;
			TargetIndex >= 0;
			--TargetIndex)
		{
			const FAssetDocumentContractValidationResult ActionResult =
				Actions[RollbackTargetIndices[TargetIndex]]->Rollback(
					StableFailureCode);
			if (!ActionResult.bValid)
			{
				Result = ActionResult;
			}
		}
		State = Result.bValid
			? EAssetDocumentAtomicState::RolledBack
			: EAssetDocumentAtomicState::Poisoned;
		Actions.Reset();
		RollbackTargetIndices.Reset();
		return Result;
	}

	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		const int32 CallIndex = EnlistCallCount++;
		ObservedComponentIds.Add(FString(Action->GetRollbackComponentId()));
		if (RejectEnlistAtCall == CallIndex)
		{
			return MakeFailure(TEXT("Test.RejectRollbackAction"));
		}
		Actions.Add(MoveTemp(Action));
		return MakeValidContractResult();
	}

	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return MakeValidContractResult();
	}

	void SetState(EAssetDocumentAtomicState InState)
	{
		State = InState;
	}

	int32 RejectEnlistAtCall = INDEX_NONE;
	int32 EnlistCallCount = 0;
	TArray<FString> ObservedComponentIds;
	TArray<FAssetDocumentMutationFootprint> EnlistedFootprint;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;

private:
	static FAssetDocumentContractValidationResult MakeFailure(
		const TCHAR* Code)
	{
		FAssetDocumentContractValidationResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/TestTransaction");
		Diagnostic.Code = Code;
		Diagnostic.Message = TEXT("The test transaction rejected the operation.");
		return Result;
	}

	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TArray<int32> RollbackTargetIndices;
};

class FStandaloneLease final : public IAssetDocumentVerifiedHandleLease
{
public:
	FStandaloneLease(
		FAssetDocumentOpaqueHandle InHandle,
		IAssetDocumentNativeHandlePayload& InPayload)
		: Handle(MoveTemp(InHandle))
		, Payload(&InPayload)
	{
	}

	virtual const FAssetDocumentOpaqueHandle& GetHandle() const override
	{
		return Handle;
	}

	virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
	{
		check(Payload != nullptr);
		return *Payload;
	}

private:
	FAssetDocumentOpaqueHandle Handle;
	IAssetDocumentNativeHandlePayload* Payload = nullptr;
};

class FWrongNativeStatePayload final : public IAssetDocumentNativeHandlePayload
{
public:
	virtual FName GetPayloadTypeId() const override
	{
		return TEXT("test.wrong.native_state_payload.v1");
	}

	virtual bool IsAlive() const override
	{
		return true;
	}
};

class FDeadEditorAssetPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	virtual FName GetPayloadTypeId() const override
	{
		return UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		return false;
	}
};

TSharedPtr<IPropertyHandle> FindPropertyHandle(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	FName PropertyName)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid() && Handle->GetProperty() != nullptr
			&& Handle->GetProperty()->GetFName() == PropertyName)
		{
			return Handle;
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		if (const TSharedPtr<IPropertyHandle> Child =
			FindPropertyHandle(Children, PropertyName))
		{
			return Child;
		}
	}
	return nullptr;
}

TSharedPtr<IPropertyHandle> FindTopLevelPropertyHandle(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	FName PropertyName)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		FProperty* Property = Handle.IsValid() ? Handle->GetProperty() : nullptr;
		if (Property != nullptr
			&& Property->GetFName() == PropertyName
			&& Property->GetOwnerProperty() == Property)
		{
			return Handle;
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		if (const TSharedPtr<IPropertyHandle> Child =
			FindTopLevelPropertyHandle(Children, PropertyName))
		{
			return Child;
		}
	}
	return nullptr;
}

TSharedPtr<IPropertyRowGenerator> MakePropertyRows(
	TConstArrayView<UObject*> Objects,
	bool bAllowMultipleObjects = false)
{
	FPropertyRowGeneratorArgs Args;
	Args.bAllowMultipleTopLevelObjects = bAllowMultipleObjects;
	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(
			TEXT("PropertyEditor"));
	TSharedRef<IPropertyRowGenerator> Rows =
		PropertyEditor.CreatePropertyRowGenerator(Args);
	Rows->OnRowsRefreshed().AddLambda([]() {});
	TArray<UObject*> SelectedObjects;
	SelectedObjects.Append(Objects.GetData(), Objects.Num());
	Rows->SetObjects(SelectedObjects);
	return Rows;
}

struct FNativeStateFixture
{
	UPackage* Package = nullptr;
	UObject* AssetAnchor = nullptr;
	UAssetDocumentEditorNativeStateTestObject* PropertyOwner = nullptr;
	UObject* OriginalReference = nullptr;
	UObject* ReplacementReference = nullptr;
	bool bOriginalDirty = false;

	void Cleanup() const
	{
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(false);
		}
		if (AssetAnchor != nullptr)
		{
			AssetAnchor->ClearFlags(RF_Public | RF_Standalone);
		}
	}
};

FNativeStateFixture MakeFixture(bool bOriginalDirty)
{
	FNativeStateFixture Fixture;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Fixture.Package = CreatePackage(*FString::Printf(
		TEXT("/Game/AssetDocumentTests/EditorNativeState/PKG_%s"),
		*Suffix));
	Fixture.AssetAnchor =
		NewObject<UAssetDocumentEditorNativeStateTestObject>(
		Fixture.Package,
		*FString::Printf(TEXT("Asset_%s"), *Suffix),
		RF_Public | RF_Standalone | RF_Transactional);
	Fixture.PropertyOwner =
		NewObject<UAssetDocumentEditorNativeStateTestObject>(
			Fixture.AssetAnchor,
			TEXT("Root"),
			RF_Transactional);
	Fixture.OriginalReference =
		NewObject<UAssetDocumentEditorNativeStateTestObject>(
		Fixture.Package,
		*FString::Printf(TEXT("Original_%s"), *Suffix));
	Fixture.ReplacementReference =
		NewObject<UAssetDocumentEditorNativeStateTestObject>(
		Fixture.Package,
		*FString::Printf(TEXT("Replacement_%s"), *Suffix));
	Fixture.PropertyOwner->AuthoredReference = Fixture.OriginalReference;
	Fixture.PropertyOwner->AuthoredReferences = {Fixture.OriginalReference};
	Fixture.PropertyOwner->OptionalReference = Fixture.OriginalReference;
	Fixture.PropertyOwner->DerivedMirror = Fixture.OriginalReference;
	Fixture.Package->SetDirtyFlag(bOriginalDirty);
	Fixture.bOriginalDirty = bOriginalDirty;
	return Fixture;
}

FAssetDocumentHandleValidationContext MakeHandleValidationContext(
	const FAssetDocumentEditorProviderTestContext& Context)
{
	FAssetDocumentHandleValidationContext Validation;
	Validation.RequestId = Context.RequestId;
	Validation.TransactionId = Context.TransactionId;
	Validation.RuntimeRevision = Context.RuntimeRevision;
	Validation.ProviderId = Context.ProviderId;
	Validation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Validation.RequiredKind = EAssetDocumentHandleKind::Asset;
	Validation.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Validation;
}

FAssetDocumentVerifiedLeaseResult MakeVerifiedAssetLease(
	FAssetDocumentEditorProviderTestHandleResolver& Resolver,
	UObject& Asset,
	const FAssetDocumentEditorProviderTestContext& Context)
{
	FAssetDocumentIssuedHandleResult Issued = Resolver.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		MakeAssetDocumentEditorAssetPayload(Asset));
	if (!Issued.Handle.IsSet())
	{
		FAssetDocumentVerifiedLeaseResult Result;
		Result.Diagnostics = MoveTemp(Issued.Diagnostics);
		return Result;
	}
	return Resolver.AcquireVerifiedLease(
		Issued.Handle.GetValue(),
		MakeHandleValidationContext(Context));
}

bool RunObjectReferenceRollbackCase(
	FAutomationTestBase& Test,
	bool bOriginalDirty,
	bool bAssertNoEditorTransaction = false,
	bool bRejectRestoreIdentity = false)
{
	FNativeStateFixture Fixture = MakeFixture(bOriginalDirty);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	FAssetDocumentVerifiedLeaseResult Lease = MakeVerifiedAssetLease(
		Resolver, *Fixture.AssetAnchor, Context);
	if (!Test.TestNotNull(TEXT("verified Asset lease is available"),
		Lease.Lease.Get()))
	{
		Fixture.Cleanup();
		return false;
	}

	TSharedPtr<IPropertyRowGenerator> Rows = MakePropertyRows(
		TArray<UObject*>({Fixture.PropertyOwner}));
	TSharedPtr<IPropertyHandle> Handle = FindPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			AuthoredReference));
	if (!Test.TestTrue(TEXT("exact object-reference handle is generated"),
		Handle.IsValid()))
	{
		Fixture.Cleanup();
		return false;
	}

	FNativeStateTestTransaction Transaction(Context);
	FAssetDocumentEditorTypedPreimageResult Capture;
	const TArray<FString> ComponentIds = {
		TEXT("rollback.editor-native-state.object"),
		TEXT("rollback.editor-native-state.package")};
	const FAssetDocumentContractValidationResult Identity =
		WithAssetDocumentEditorAssetIdentity(
			*Lease.Lease,
			[&](const FAssetDocumentEditorAssetIdentityView& View)
			{
				Capture =
					CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
						View,
						*Fixture.PropertyOwner,
						Rows.ToSharedRef(),
						Handle.ToSharedRef(),
						ComponentIds,
						Transaction);
				return MakeCaptureContractResult(Capture);
			});
	if (!Test.TestTrue(TEXT("typed preimage capture succeeds"),
		Identity.bValid && Capture.bSucceeded))
	{
		Fixture.Cleanup();
		return false;
	}
	Test.TestEqual(TEXT("both alias actions enlist in caller order"),
		Transaction.ObservedComponentIds, ComponentIds);

	const TArray<FAssetDocumentMutationFootprint> Footprint = {
		{EAssetDocumentMutationFootprintKind::UObjectState,
			TEXT("native-state.object"), ComponentIds[0]},
		{EAssetDocumentMutationFootprintKind::Package,
			TEXT("native-state.package"), ComponentIds[1]}};
	if (!Test.TestTrue(TEXT("test footprint enlistment succeeds"),
		Transaction.EnlistFootprint(Footprint).bValid))
	{
		Fixture.Cleanup();
		return false;
	}
	const UObject* const ReplacementReference = Fixture.ReplacementReference;
	if (!Test.TestTrue(TEXT("authoritative typed mutation succeeds"),
		Handle->SetValue(
			ReplacementReference,
			EPropertyValueSetFlags::NotTransactable)
		== FPropertyAccess::Success))
	{
		Fixture.Cleanup();
		return false;
	}
	Test.TestEqual(TEXT("authored value changed"),
		Fixture.PropertyOwner->AuthoredReference.Get(),
		Fixture.ReplacementReference);
	Test.TestEqual(TEXT("native PostEdit derived mirror changed"),
		Fixture.PropertyOwner->DerivedMirror.Get(),
		Fixture.ReplacementReference);
	Test.TestEqual(TEXT("mutation emits one native lifecycle call"),
		Fixture.PropertyOwner->AuthoredReferencePostEditCount, 1);

	Rows->SetObjects({Fixture.PropertyOwner});
	Handle.Reset();
	Rows.Reset();
	const int32 EditorTransactionCountBeforeRollback =
		bAssertNoEditorTransaction
			? (GEditor != nullptr && GEditor->Trans != nullptr
				? GEditor->Trans->GetQueueLength()
				: INDEX_NONE)
			: INDEX_NONE;
	if (bAssertNoEditorTransaction
		&& !Test.TestTrue(
			TEXT("Editor transaction authority is available"),
			EditorTransactionCountBeforeRollback != INDEX_NONE))
	{
		Fixture.Cleanup();
		return false;
	}
	// Keep both aliases alive after execution, as the real kernel does.
	const TArray<TSharedRef<IAssetDocumentRollbackAction>> RetainedActions = Transaction.Actions;
	if (bRejectRestoreIdentity)
	{
		if (!Test.TestTrue(TEXT("fixture identity changes before restore"),
			Fixture.PropertyOwner->Rename(TEXT("RenamedRoot"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional)))
		{
			Fixture.Cleanup();
			return false;
		}
	}
	const FAssetDocumentContractValidationResult Rollback =
		Transaction.Rollback(TEXTVIEW("Test.NativeState.Rollback"));
	if (bRejectRestoreIdentity)
	{
		Test.TestFalse(TEXT("changed identity rejects restore"), Rollback.bValid);
		Fixture.Cleanup();
		Lease.Lease.Reset();
		FAssetDocumentHandleInvalidation Event;
		Event.RequestId = Context.RequestId;
		Event.TransactionId = Context.TransactionId;
		Event.RuntimeRevision = Context.RuntimeRevision;
		Event.ProviderId = Context.ProviderId;
		Resolver.Invalidate(Event);
		TWeakObjectPtr<UObject> WeakAsset(Fixture.AssetAnchor);
		TWeakObjectPtr<UObject> WeakOwner(Fixture.PropertyOwner);
		TWeakObjectPtr<UObject> WeakReference(Fixture.OriginalReference);
		TWeakObjectPtr<UPackage> WeakPackage(Fixture.Package);
		CollectGarbage(RF_NoFlags);
		Test.TestTrue(TEXT("failed restore retains asset owner reference and package"),
			WeakAsset.IsValid() && WeakOwner.IsValid() && WeakReference.IsValid() && WeakPackage.IsValid());
		for (const auto& Action : RetainedActions)
		{
			const auto Repeated = Action->Rollback(TEXTVIEW("Test.NativeState.RepeatedFailure"));
			Test.TestFalse(TEXT("each alias preserves cached failure"), Repeated.bValid);
			if (Test.TestTrue(TEXT("failure diagnostics remain available"),
				!Repeated.Diagnostics.IsEmpty() && !Rollback.Diagnostics.IsEmpty()))
			{
				Test.TestEqual(TEXT("cached failure diagnostic remains exact"),
					Repeated.Diagnostics[0].Code, Rollback.Diagnostics[0].Code);
			}
		}
		if (WeakOwner.IsValid())
		{
			Test.TestEqual(TEXT("failed aliases emit no restore lifecycle"),
				Fixture.PropertyOwner->AuthoredReferencePostEditCount, 1);
		}
		return !Rollback.bValid;
	}
	Test.TestTrue(TEXT("rollback succeeds after caller releases row authority"),
		Rollback.bValid);
	Test.TestEqual(TEXT("authored value restores"),
		Fixture.PropertyOwner->AuthoredReference.Get(),
		Fixture.OriginalReference);
	Test.TestEqual(TEXT("native PostEdit derived mirror restores"),
		Fixture.PropertyOwner->DerivedMirror.Get(),
		Fixture.OriginalReference);
	Test.TestEqual(TEXT("alias state restores through typed lifecycle once"),
		Fixture.PropertyOwner->AuthoredReferencePostEditCount, 2);
	Test.TestEqual(TEXT("package dirty preimage restores exactly"),
		Fixture.Package->IsDirty(), Fixture.bOriginalDirty);
	if (bAssertNoEditorTransaction)
	{
		Test.TestEqual(
			TEXT("outer Atomic rollback creates no Editor transaction"),
			GEditor->Trans->GetQueueLength(),
			EditorTransactionCountBeforeRollback);
	}
	Fixture.Cleanup();
	Lease.Lease.Reset();
	FAssetDocumentHandleInvalidation Event;
	Event.RequestId = Context.RequestId;
	Event.TransactionId = Context.TransactionId;
	Event.RuntimeRevision = Context.RuntimeRevision;
	Event.ProviderId = Context.ProviderId;
	Resolver.Invalidate(Event);
	TWeakObjectPtr<UObject> WeakAsset(Fixture.AssetAnchor);
	TWeakObjectPtr<UObject> WeakOwner(Fixture.PropertyOwner);
	TWeakObjectPtr<UObject> WeakReference(Fixture.OriginalReference);
	TWeakObjectPtr<UPackage> WeakPackage(Fixture.Package);
	CollectGarbage(RF_NoFlags);
	Test.TestFalse(TEXT("successful restore releases asset while aliases remain alive"), WeakAsset.IsValid());
	Test.TestFalse(TEXT("successful restore releases property owner"), WeakOwner.IsValid());
	Test.TestFalse(TEXT("successful restore releases original reference"), WeakReference.IsValid());
	Test.TestFalse(TEXT("successful restore releases package"), WeakPackage.IsValid());
	for (const auto& Action : RetainedActions)
	{
		Test.TestTrue(TEXT("each restored alias remains idempotent after collection"),
			Action->Rollback(TEXTVIEW("Test.NativeState.RepeatedSuccess")).bValid);
	}
	return Rollback.bValid;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorTypedAssetIdentityLeaseTest,
	"UEAssetDoc.EditorNativeState.TypedAssetIdentityLease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorTypedAssetIdentityLeaseTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FNativeStateFixture Fixture = MakeFixture(false);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	FAssetDocumentVerifiedLeaseResult Lease = MakeVerifiedAssetLease(
		Resolver, *Fixture.AssetAnchor, Context);
	if (!TestNotNull(TEXT("verified Asset lease is available"), Lease.Lease.Get()))
	{
		Fixture.Cleanup();
		return false;
	}

	int32 CallbackCount = 0;
	const FAssetDocumentContractValidationResult Result =
		WithAssetDocumentEditorAssetIdentity(
			*Lease.Lease,
			[&](const FAssetDocumentEditorAssetIdentityView& View)
			{
				++CallbackCount;
				TestEqual(TEXT("callback receives exact native object"),
					&View.GetObject(), Fixture.AssetAnchor);
				TestEqual(TEXT("callback receives exact canonical path"),
					FString(View.GetCanonicalObjectPath()),
					Fixture.AssetAnchor->GetPathName());
				TestEqual(TEXT("callback receives exact type revision"),
					View.GetTypeRevision(),
					static_cast<uint64>(Fixture.AssetAnchor->GetUniqueID()));
				return MakeValidContractResult();
			});
	TestTrue(TEXT("typed Asset identity succeeds"), Result.bValid);
	TestEqual(TEXT("identity callback executes once"), CallbackCount, 1);
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorTypedAssetIdentityRejectsUnsupportedTest,
	"UEAssetDoc.EditorNativeState.TypedAssetIdentityRejectsUnsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorTypedAssetIdentityRejectsUnsupportedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FNativeStateFixture Fixture = MakeFixture(false);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentOpaqueHandle Handle;
	Handle.RequestId = Context.RequestId;
	Handle.TransactionId = Context.TransactionId;
	Handle.RuntimeRevision = Context.RuntimeRevision;
	Handle.ProviderId = Context.ProviderId;
	Handle.Slot = 1;
	Handle.Generation = 1;
	Handle.Kind = EAssetDocumentHandleKind::Asset;
	Handle.Access = EAssetDocumentHandleAccess::Mutable;

	int32 CallbackCount = 0;
	auto Consumer = [&](const FAssetDocumentEditorAssetIdentityView&)
	{
		++CallbackCount;
		return MakeValidContractResult();
	};

	TUniquePtr<IAssetDocumentNativeHandlePayload> RealPayload =
		MakeAssetDocumentEditorAssetPayload(*Fixture.AssetAnchor);
	FAssetDocumentOpaqueHandle WrongKindHandle = Handle;
	WrongKindHandle.Kind = EAssetDocumentHandleKind::Object;
	FStandaloneLease WrongKind(WrongKindHandle, *RealPayload);
	const FAssetDocumentContractValidationResult WrongKindResult =
		WithAssetDocumentEditorAssetIdentity(WrongKind, Consumer);
	TestFalse(TEXT("non-Asset lease fails"), WrongKindResult.bValid);
	TestTrue(TEXT("non-Asset failure is stable"),
		HasDiagnostic(WrongKindResult.Diagnostics,
			TEXTVIEW("EditorNativeState.AssetHandleKindRequired")));

	FWrongNativeStatePayload WrongPayload;
	FStandaloneLease WrongType(Handle, WrongPayload);
	const FAssetDocumentContractValidationResult WrongTypeResult =
		WithAssetDocumentEditorAssetIdentity(WrongType, Consumer);
	TestFalse(TEXT("foreign payload family fails"), WrongTypeResult.bValid);
	TestTrue(TEXT("foreign payload failure is stable"),
		HasDiagnostic(WrongTypeResult.Diagnostics,
			TEXTVIEW("EditorNativeState.AssetPayloadTypeMismatch")));

	FDeadEditorAssetPayload DeadPayload;
	FStandaloneLease Dead(Handle, DeadPayload);
	const FAssetDocumentContractValidationResult DeadResult =
		WithAssetDocumentEditorAssetIdentity(Dead, Consumer);
	TestFalse(TEXT("dead Asset payload fails"), DeadResult.bValid);
	TestTrue(TEXT("dead Asset failure is stable"),
		HasDiagnostic(DeadResult.Diagnostics,
			TEXTVIEW("EditorNativeState.AssetPayloadNotAlive")));
	TestEqual(TEXT("all failures occur before callback"), CallbackCount, 0);
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorObjectReferencePreimageRollbackTest,
	"UEAssetDoc.EditorNativeState.ObjectReferencePreimageRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorObjectReferencePreimageRollbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const bool bCleanCase = RunObjectReferenceRollbackCase(*this, false);
	const bool bDirtyCase = RunObjectReferenceRollbackCase(*this, true);
	const bool bNoEditorTransactionCase =
		RunObjectReferenceRollbackCase(*this, false, true);
	const bool bFailedRestoreKeepsPreimage =
		RunObjectReferenceRollbackCase(*this, false, false, true);
	return bCleanCase && bDirtyCase && bNoEditorTransactionCase && bFailedRestoreKeepsPreimage;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorObjectReferencePreimageRejectsUnsupportedTest,
	"UEAssetDoc.EditorNativeState.ObjectReferencePreimageRejectsUnsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorObjectReferencePreimageRejectsUnsupportedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FNativeStateFixture Fixture = MakeFixture(false);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	FAssetDocumentVerifiedLeaseResult Lease = MakeVerifiedAssetLease(
		Resolver, *Fixture.AssetAnchor, Context);
	if (!TestNotNull(TEXT("verified Asset lease is available"), Lease.Lease.Get()))
	{
		Fixture.Cleanup();
		return false;
	}

	TSharedPtr<IPropertyRowGenerator> Rows = MakePropertyRows(
		TArray<UObject*>({Fixture.PropertyOwner}));
	TSharedPtr<IPropertyHandle> ObjectHandle = FindPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			AuthoredReference));
	TSharedPtr<IPropertyHandle> ScalarHandle = FindPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			ScalarValue));
	TSharedPtr<IPropertyHandle> ArrayContainerHandle =
		FindTopLevelPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			AuthoredReferences));
	TSharedPtr<IPropertyHandle> OptionalInnerHandle = FindPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			OptionalReference));
	TSharedPtr<IPropertyHandle> ArrayElementHandle;
	if (ArrayContainerHandle.IsValid()
		&& ArrayContainerHandle->AsArray().IsValid())
	{
		ArrayElementHandle = ArrayContainerHandle->AsArray()->GetElement(0);
	}
	TSharedPtr<IPropertyHandle> ClassHandle = FindPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			ClassReference));
	TSharedPtr<IPropertyHandle> PackageHandle = FindPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			PackageReference));
	TSharedPtr<IPropertyHandle> GraphHandle = FindPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			GraphReference));
	TSharedPtr<IPropertyHandle> NodeHandle = FindPropertyHandle(
		Rows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			NodeReference));
	const bool bAllNegativeHandlesGenerated =
		TestTrue(TEXT("object handle is generated"), ObjectHandle.IsValid())
		&& TestTrue(TEXT("scalar handle is generated"), ScalarHandle.IsValid())
		&& TestTrue(TEXT("array container handle is generated"),
			ArrayContainerHandle.IsValid())
		&& TestTrue(TEXT("array element handle is generated"),
			ArrayElementHandle.IsValid())
		&& TestTrue(TEXT("optional inner handle is generated"),
			OptionalInnerHandle.IsValid())
		&& TestTrue(TEXT("Class handle is generated"), ClassHandle.IsValid())
		&& TestTrue(TEXT("Package handle is generated"), PackageHandle.IsValid())
		&& TestTrue(TEXT("Graph handle is generated"), GraphHandle.IsValid())
		&& TestTrue(TEXT("Node handle is generated"), NodeHandle.IsValid());
	if (!bAllNegativeHandlesGenerated)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("array element carries container owner ancestry"),
		ArrayElementHandle->GetProperty()->GetOwnerProperty()
			!= ArrayElementHandle->GetProperty());
	TestTrue(TEXT("optional value carries container owner ancestry"),
		OptionalInnerHandle->GetProperty()->GetOwnerProperty()
			!= OptionalInnerHandle->GetProperty());
	TestTrue(TEXT("array element handle belongs to exact rows"),
		Rows->FindTreeNode(ArrayElementHandle).IsValid());
	TestTrue(TEXT("optional value handle belongs to exact rows"),
		Rows->FindTreeNode(OptionalInnerHandle).IsValid());

	const TArray<FString> Components = {
		TEXT("rollback.editor-native-state.object"),
		TEXT("rollback.editor-native-state.package")};
	FNativeStateTestTransaction MatchingTransaction(Context);
	FAssetDocumentEditorProviderTestContext ForeignContext;
	FNativeStateTestTransaction ForeignTransaction(ForeignContext);
	FNativeStateTestTransaction InvalidStateTransaction(Context);
	InvalidStateTransaction.SetState(
		EAssetDocumentAtomicState::ReadyToCommit);
	FNativeStateTestTransaction PartialTransaction(Context);
	PartialTransaction.RejectEnlistAtCall = 1;
	FAssetDocumentEditorTypedPreimageResult ForeignCapture;
	FAssetDocumentEditorTypedPreimageResult ScalarCapture;
	FAssetDocumentEditorTypedPreimageResult InvalidStateCapture;
	FAssetDocumentEditorTypedPreimageResult DuplicateCapture;
	FAssetDocumentEditorTypedPreimageResult PartialCapture;
	const bool bDirtyBeforeUnsupportedCapture = Fixture.Package->IsDirty();
	auto TestUnsupportedFamily = [&](const TCHAR* Family,
		const FAssetDocumentEditorAssetIdentityView& View,
		const TSharedPtr<IPropertyHandle>& UnsupportedHandle)
	{
		FNativeStateTestTransaction UnsupportedTransaction(Context);
		const FAssetDocumentEditorTypedPreimageResult UnsupportedCapture =
			CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
				View, *Fixture.PropertyOwner, Rows.ToSharedRef(),
				UnsupportedHandle.ToSharedRef(), Components,
				UnsupportedTransaction);
		TestFalse(*FString::Printf(TEXT("%s family fails closed"), Family),
			UnsupportedCapture.bSucceeded);
		TestTrue(*FString::Printf(TEXT("%s diagnostic is stable"), Family),
			HasDiagnostic(UnsupportedCapture.Diagnostics,
				TEXTVIEW("EditorNativeState.ObjectReferencePropertyRequired")));
		TestEqual(*FString::Printf(TEXT("%s enlists zero actions"), Family),
			UnsupportedTransaction.Actions.Num(), 0);
		TestTrue(*FString::Printf(TEXT("%s enlists zero footprint"), Family),
			UnsupportedTransaction.EnlistedFootprint.IsEmpty());
		TestEqual(*FString::Printf(TEXT("%s preserves dirty preimage"), Family),
			Fixture.Package->IsDirty(), bDirtyBeforeUnsupportedCapture);
		if (!UnsupportedTransaction.Actions.IsEmpty())
		{
			UnsupportedTransaction.Rollback(
				TEXTVIEW("Test.UnsupportedFamilyCleanup"));
		}
	};

	const FAssetDocumentContractValidationResult Identity =
		WithAssetDocumentEditorAssetIdentity(
			*Lease.Lease,
			[&](const FAssetDocumentEditorAssetIdentityView& View)
			{
				ForeignCapture =
					CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
						View, *Fixture.PropertyOwner, Rows.ToSharedRef(),
						ObjectHandle.ToSharedRef(), Components,
						ForeignTransaction);
				ScalarCapture =
					CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
						View, *Fixture.PropertyOwner, Rows.ToSharedRef(),
						ScalarHandle.ToSharedRef(), Components,
						MatchingTransaction);
				InvalidStateCapture =
					CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
						View, *Fixture.PropertyOwner, Rows.ToSharedRef(),
						ObjectHandle.ToSharedRef(), Components,
						InvalidStateTransaction);
				DuplicateCapture =
					CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
						View, *Fixture.PropertyOwner, Rows.ToSharedRef(),
						ObjectHandle.ToSharedRef(),
						TArray<FString>({TEXT("duplicate"), TEXT("duplicate")}),
						MatchingTransaction);
				PartialCapture =
					CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
						View, *Fixture.PropertyOwner, Rows.ToSharedRef(),
						ObjectHandle.ToSharedRef(), Components,
						PartialTransaction);
				TestUnsupportedFamily(
					TEXT("container element"), View, ArrayElementHandle);
				TestUnsupportedFamily(
					TEXT("optional value"), View, OptionalInnerHandle);
				TestUnsupportedFamily(TEXT("Class"), View, ClassHandle);
				TestUnsupportedFamily(TEXT("Package"), View, PackageHandle);
				TestUnsupportedFamily(TEXT("Graph"), View, GraphHandle);
				TestUnsupportedFamily(TEXT("Node"), View, NodeHandle);
				return MakeValidContractResult();
			});
	TestTrue(TEXT("identity callback remains valid"), Identity.bValid);
	TestFalse(TEXT("foreign transaction binding fails"),
		ForeignCapture.bSucceeded);
	TestTrue(TEXT("foreign transaction diagnostic is stable"),
		HasDiagnostic(ForeignCapture.Diagnostics,
			TEXTVIEW("EditorNativeState.TransactionBindingMismatch")));
	TestFalse(TEXT("scalar property family fails"), ScalarCapture.bSucceeded);
	TestTrue(TEXT("scalar family diagnostic is stable"),
		HasDiagnostic(ScalarCapture.Diagnostics,
			TEXTVIEW("EditorNativeState.ObjectReferencePropertyRequired")));
	TestFalse(TEXT("non-live transaction state fails"),
		InvalidStateCapture.bSucceeded);
	TestTrue(TEXT("transaction state diagnostic is stable"),
		HasDiagnostic(InvalidStateCapture.Diagnostics,
			TEXTVIEW("EditorNativeState.TransactionStateInvalid")));
	TestFalse(TEXT("duplicate rollback components fail"),
		DuplicateCapture.bSucceeded);
	TestTrue(TEXT("duplicate component diagnostic is stable"),
		HasDiagnostic(DuplicateCapture.Diagnostics,
			TEXTVIEW("EditorNativeState.RollbackComponentInvalid")));
	TestFalse(TEXT("second alias rejection fails capture"),
		PartialCapture.bSucceeded);
	TestTrue(TEXT("partial alias diagnostic is stable"),
		HasDiagnostic(PartialCapture.Diagnostics,
			TEXTVIEW("EditorNativeState.RollbackActionEnlistmentFailed")));
	TestEqual(TEXT("partial enlist observes caller order"),
		PartialTransaction.ObservedComponentIds, Components);
	TestEqual(TEXT("one alias was accepted before second rejection"),
		PartialTransaction.Actions.Num(), 1);
	TestTrue(TEXT("failed capture enlists no footprint"),
		PartialTransaction.EnlistedFootprint.IsEmpty());
	TestEqual(TEXT("failed capture leaves authored value unchanged"),
		Fixture.PropertyOwner->AuthoredReference.Get(),
		Fixture.OriginalReference);
	TestEqual(TEXT("failed capture leaves derived value unchanged"),
		Fixture.PropertyOwner->DerivedMirror.Get(),
		Fixture.OriginalReference);
	TestEqual(TEXT("failed capture emits no property lifecycle call"),
		Fixture.PropertyOwner->AuthoredReferencePostEditCount, 0);
	TestFalse(TEXT("failed capture leaves package clean"),
		Fixture.Package->IsDirty());
	TestEqual(TEXT("container value remains unchanged"),
		Fixture.PropertyOwner->AuthoredReferences[0].Get(),
		Fixture.OriginalReference);
	TestEqual(TEXT("optional value remains unchanged"),
		Fixture.PropertyOwner->OptionalReference.GetValue().Get(),
		Fixture.OriginalReference);
	TestNull(TEXT("Class value remains unchanged"),
		Fixture.PropertyOwner->ClassReference.Get());
	TestNull(TEXT("Package value remains unchanged"),
		Fixture.PropertyOwner->PackageReference.Get());
	TestNull(TEXT("Graph value remains unchanged"),
		Fixture.PropertyOwner->GraphReference.Get());
	TestNull(TEXT("Node value remains unchanged"),
		Fixture.PropertyOwner->NodeReference.Get());
	const FAssetDocumentContractValidationResult Cleanup =
		PartialTransaction.Rollback(TEXTVIEW("Test.PartialCleanup"));
	TestTrue(TEXT("unbound partial action cleanup succeeds"), Cleanup.bValid);
	TestEqual(TEXT("unbound partial action never executes"),
		Fixture.PropertyOwner->AuthoredReferencePostEditCount, 0);

	UAssetDocumentEditorNativeStateTestObject* OtherOwner =
		NewObject<UAssetDocumentEditorNativeStateTestObject>(
			Fixture.AssetAnchor, TEXT("OtherRoot"), RF_Transactional);
	TSharedPtr<IPropertyRowGenerator> MultiRows = MakePropertyRows(
		TArray<UObject*>({Fixture.PropertyOwner, OtherOwner}), true);
	TSharedPtr<IPropertyHandle> MultiHandle = FindPropertyHandle(
		MultiRows->GetRootTreeNodes(),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentEditorNativeStateTestObject,
			AuthoredReference));
	if (TestTrue(TEXT("multi-object handle is generated"), MultiHandle.IsValid()))
	{
		FNativeStateTestTransaction MultiTransaction(Context);
		FAssetDocumentEditorTypedPreimageResult MultiCapture;
		WithAssetDocumentEditorAssetIdentity(
			*Lease.Lease,
			[&](const FAssetDocumentEditorAssetIdentityView& View)
			{
				MultiCapture =
					CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
						View, *Fixture.PropertyOwner, MultiRows.ToSharedRef(),
						MultiHandle.ToSharedRef(), Components,
						MultiTransaction);
				return MakeValidContractResult();
			});
		TestFalse(TEXT("multi-object row authority fails closed"),
			MultiCapture.bSucceeded);
		TestTrue(TEXT("multi-object failure is stable"),
			HasDiagnostic(MultiCapture.Diagnostics,
				TEXTVIEW("EditorNativeState.PropertyRowOwnerMismatch")));
	}
	Fixture.Cleanup();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
