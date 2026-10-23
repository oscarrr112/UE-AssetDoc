// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentEditorSequenceOwnedFragmentProperties.h"
#include "AssetDocumentEditorSequenceOwnedFragmentPropertyTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetDocumentRollbackAction.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Misc/AutomationTest.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
TWeakObjectPtr<UObject> SequenceFragmentDesiredSeed;
}
FAssetDocumentEditorSequenceFragmentDesiredReferenceTest::FAssetDocumentEditorSequenceFragmentDesiredReferenceTest()
	: Reference(SequenceFragmentDesiredSeed.Get()) {}
void FAssetDocumentEditorSequenceFragmentDesiredReferenceTest::SetDefaultReference(UObject* Object)
{
	SequenceFragmentDesiredSeed = Object;
}
void FAssetDocumentEditorSequenceFragmentDesiredReferenceTest::AddStructReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Reference);
}

#if WITH_EDITOR
void UAssetDocumentEditorSequenceFragmentMetadataTest::PreEditChange(FProperty* P)
{
	++PreEditCount; Super::PreEditChange(P);
}
void UAssetDocumentEditorSequenceFragmentMetadataTest::PostEditChangeProperty(FPropertyChangedEvent& E)
{
	++PostEditCount; ++ListenerEffect; Super::PostEditChangeProperty(E);
}
void UAssetDocumentEditorSequenceFragmentUserDataTest::PostEditChangeProperty(FPropertyChangedEvent& E)
{
	++PostEditCount; Super::PostEditChangeProperty(E);
}
#endif
void UAssetDocumentEditorSequenceFragmentUserDataTest::PostEditChangeOwner(const FPropertyChangedEvent&)
{
	++OwnerEditCount;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorSequenceOwnedFragmentTests
{
FAssetDocumentRuntimeValueDto Integer(int32 N)
{
	FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Integer; V.IntegerValue = N; return V;
}
FAssetDocumentRuntimeValueDto String(FString S)
{
	FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = MoveTemp(S); return V;
}
FAssetDocumentRuntimeValueDto Properties()
{
	FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Object;
	V.ObjectKeys = {TEXT("First"), TEXT("Second")}; V.Elements = {Integer(17), Integer(29)}; return V;
}
struct FFixture
{
	FAssetDocumentEditorProviderTestContext Binding;
	FAssetDocumentEditorSequenceFragmentContext Context;
	TUniquePtr<FAssetDocumentEditorProviderTestHandleResolver> Resolver;
	TUniquePtr<FAssetDocumentEditorProviderTestTransaction> Transaction;
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UAnimSequence> Parent;
	TStrongObjectPtr<UAssetDocumentEditorSequenceFragmentMetadataTest> Metadata;
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease;
	explicit FFixture(bool bEnlistParent = true)
	{
		Binding.ProviderId = TEXT("animation");
		Context.OperationId = FGuid::NewGuid(); Context.Mutation = {Binding.RequestId, Binding.TransactionId, Binding.RuntimeRevision};
		Resolver = MakeUnique<FAssetDocumentEditorProviderTestHandleResolver>(Binding);
		Transaction = MakeUnique<FAssetDocumentEditorProviderTestTransaction>(Binding);
		Package.Reset(CreatePackage(*(TEXT("/Temp/SequenceFragment_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		Parent.Reset(NewObject<UAnimSequence>(Package.Get(), TEXT("Sequence"), RF_Public | RF_Transactional));
		Metadata.Reset(NewObject<UAssetDocumentEditorSequenceFragmentMetadataTest>(Parent.Get(), TEXT("Metadata"), RF_Transactional));
		Parent->AddMetaData(Metadata.Get());
		Metadata->Values = {2, 4};
		Lease = Acquire(*Parent.Get());
		const TArray<FAssetDocumentMutationFootprint> ParentFootprint = {
			{EAssetDocumentMutationFootprintKind::UObjectState, Parent->GetPathName(), TEXT("test.parent.inverse")}};
		if (bEnlistParent) Transaction->EnlistFootprint(ParentFootprint);
		Package->SetDirtyFlag(false);
	}
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Acquire(UObject& Object,
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::Mutable)
	{
		auto Issued = Resolver->Issue(EAssetDocumentHandleKind::Asset, Access, MakeAssetDocumentEditorAssetPayload(Object));
		if (!Issued.Handle.IsSet()) return nullptr;
		FAssetDocumentHandleValidationContext C;
		C.RequestId = Binding.RequestId; C.TransactionId = Binding.TransactionId; C.RuntimeRevision = Binding.RuntimeRevision;
		C.ProviderId = Binding.ProviderId; C.Phase = Context.Phase; C.RequiredKind = EAssetDocumentHandleKind::Asset; C.RequiredAccess = Access;
		auto Result = Resolver->AcquireVerifiedLease(*Issued.Handle, C); return MoveTemp(Result.Lease);
	}
	TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties> Prepare(
		const FAssetDocumentRuntimeValueDto& P, TArray<FAssetDocumentRuntimeDiagnostic>& D)
	{
		return PrepareAssetDocumentEditorSequenceOwnedFragmentProperties(Context, *Lease, *Resolver, *Metadata.Get(), P, *Transaction, D);
	}
	TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentClassPlan> ClassPlan(
		const FAssetDocumentRuntimeValueDto& P, TArray<FAssetDocumentRuntimeDiagnostic>& D)
	{
		return PrepareAssetDocumentEditorSequenceOwnedFragmentClassPlan(Context, *Lease, *Resolver,
			*UAssetDocumentEditorSequenceFragmentMetadataTest::StaticClass(), P, *Transaction, D);
	}
};
struct FNotifications
{
	UObject* Child;
	UObject* Parent;
	int32 Changed = 0, Modified = 0;
	FDelegateHandle ChangedHandle, ModifiedHandle;
	FNotifications(UObject& C, UObject& P) : Child(&C), Parent(&P)
	{
		ChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject* O, FPropertyChangedEvent&)
			{ if (O == Child || O == Parent) ++Changed; });
		ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject* O)
			{ if (O == Child || O == Parent) ++Modified; });
	}
	~FNotifications()
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ChangedHandle);
		FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
	}
};
class FCancellation final : public IAssetDocumentCancellationView
{
public:
	bool IsCancellationRequested() const override { return false; }
};
// The public Core composition invalidates all request providers (empty provider
// id). Adapt the existing test registry's provider filter, not Core's algorithm.
class FRequestResolver final : public IAssetDocumentHandleResolver
{
public:
	explicit FRequestResolver(const FAssetDocumentEditorProviderTestContext& C) : Binding(C)
	{ Inner = MakeUnique<FAssetDocumentEditorProviderTestHandleResolver>(Binding); }
	FAssetDocumentEditorProviderTestContext Binding;
	TUniquePtr<FAssetDocumentEditorProviderTestHandleResolver> Inner;
	int32 RollbackInvalidationCount = 0;
	uint32 InvalidatedHandles = 0;
	FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind K, EAssetDocumentHandleAccess A,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& P) override { return Inner->Issue(K, A, MoveTemp(P)); }
	FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& H,
		const FAssetDocumentHandleValidationContext& C) override { return Inner->AcquireVerifiedLease(H, C); }
	FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& E) override
	{
		auto ProviderEvent = E;
		if (ProviderEvent.ProviderId.IsEmpty()) ProviderEvent.ProviderId = Binding.ProviderId;
		const auto Result = Inner->Invalidate(ProviderEvent);
		if (E.Reason == EAssetDocumentHandleInvalidationReason::Rollback)
		{ ++RollbackInvalidationCount; InvalidatedHandles += Result.InvalidatedHandleCount; }
		return Result;
	}
};
class FNativeInverseAction final : public IAssetDocumentRollbackAction
{
public:
	FAssetDocumentEditorSequenceFragmentContext Context;
	TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties> Prepared;
	IAssetDocumentAtomicTransaction* Transaction = nullptr;
	IAssetDocumentHandleResolver* Resolver = nullptr;
	FAssetDocumentOpaqueHandle Handle;
	FAssetDocumentHandleValidationContext Validation;
	bool bRevokedBeforeAction = false;
	int32 Calls = 0;
	FStringView GetRollbackComponentId() const override { return TEXT("test.parent.inverse"); }
	FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		++Calls;
		if (!Prepared) { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
		bRevokedBeforeAction = !Resolver->AcquireVerifiedLease(Handle, Validation).Lease;
		return Prepared->RestoreRaw(Context, *Transaction);
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentRawRoundtrip,
	"UEAssetDoc.Editor.SequenceOwnedFragment.RawRoundtripNoEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentRawRoundtrip::RunTest(const FString&)
{
	FFixture F; if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	// Positive listener control: this real default-notify setter reaches the
	// callbacks whose absence is required from both raw directions below.
	FProperty* First = FindFProperty<FProperty>(F.Metadata->GetClass(), TEXT("First"));
	FNotifications Events(*F.Metadata.Get(), *F.Parent.Get());
	int32 Control = 4;
	TestTrue(TEXT("Default setter control"), PropertyAccessUtil::SetPropertyValue_Object(First, F.Metadata.Get(), First, &Control,
		INDEX_NONE, PropertyAccessUtil::EditorReadOnlyFlags, EPropertyAccessChangeNotifyMode::Default) == EPropertyAccessResultFlags::Success);
	TestTrue(TEXT("listeners really observe notifying setter"), F.Metadata->PreEditCount > 0 && F.Metadata->PostEditCount > 0 && Events.Changed > 0);
	F.Metadata->First = 3;
	F.Metadata->PreEditCount = F.Metadata->PostEditCount = F.Metadata->ListenerEffect = 0; Events.Changed = Events.Modified = 0;
	F.Package->SetDirtyFlag(false);
	auto P = Properties();
	FAssetDocumentRuntimeValueDto Array; Array.Kind = EAssetDocumentRuntimeValueKind::Array; Array.Elements = {Integer(8), Integer(9), Integer(10)};
	P.ObjectKeys.Add(TEXT("Values")); P.Elements.Add(Array);
	Array.Elements = {Integer(31), Integer(41)};
	P.ObjectKeys.Add(TEXT("StaticValues")); P.Elements.Add(Array);
	P.ObjectKeys.Add(TEXT("SoftReference")); P.Elements.Add(String(TEXT("/Game/Untouched/Target.Target")));
	TArray<FAssetDocumentRuntimeDiagnostic> D; auto Prepared = F.Prepare(P, D);
	if (!TestTrue(TEXT("prepare captures and decodes"), Prepared.IsValid())) return false;
	TestEqual(TEXT("prepare writes nothing"), F.Metadata->First, 3);
	TestTrue(TEXT("raw apply"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	TestEqual(TEXT("first value"), F.Metadata->First, 17); TestEqual(TEXT("second value"), F.Metadata->Second, 29);
	TestTrue(TEXT("container typed value"), F.Metadata->Values == TArray<int32>({8, 9, 10}));
	TestEqual(TEXT("first static-array value"), F.Metadata->StaticValues[0], 31); TestEqual(TEXT("last static-array value"), F.Metadata->StaticValues[1], 41);
	TestEqual(TEXT("soft reference value"), F.Metadata->SoftReference.ToSoftObjectPath().ToString(), FString(TEXT("/Game/Untouched/Target.Target")));
	auto Read = ReadAssetDocumentEditorSequenceOwnedFragmentProperties(F.Context, *F.Lease, *F.Resolver, *F.Metadata.Get(), P.ObjectKeys);
	TestTrue(TEXT("native typed read"), Read.bSucceeded); TestEqual(TEXT("all requested values"), Read.Properties.Elements.Num(), 5);
	if (Read.Properties.Elements.Num() == 5)
	{
		TestEqual(TEXT("reader returns actual scalar"), Read.Properties.Elements[0].IntegerValue, int64(17));
		TestEqual(TEXT("reader returns actual soft path"), Read.Properties.Elements[4].StringValue, FString(TEXT("/Game/Untouched/Target.Target")));
	}
	auto Wrong = F.Context; Wrong.OperationId = FGuid::NewGuid();
	TestFalse(TEXT("cross-operation inverse rejected"), Prepared->RestoreRaw(Wrong, *F.Transaction).bValid);
	FAssetDocumentEditorProviderTestTransaction SameTokenDifferentTransaction(F.Binding);
	const TArray<FAssetDocumentMutationFootprint> OtherFootprint = {{EAssetDocumentMutationFootprintKind::UObjectState, F.Parent->GetPathName(), TEXT("test.parent.inverse")}};
	SameTokenDifferentTransaction.EnlistFootprint(OtherFootprint);
	TestFalse(TEXT("a different transaction object cannot consume inverse even with same token"), Prepared->RestoreRaw(F.Context, SameTokenDifferentTransaction).bValid);
	TestEqual(TEXT("invalid inverse writes nothing"), F.Metadata->First, 17);
	TestTrue(TEXT("exact field inverse"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid);
	TestEqual(TEXT("first restored"), F.Metadata->First, 3); TestEqual(TEXT("second restored"), F.Metadata->Second, 5);
	TestTrue(TEXT("container restored"), F.Metadata->Values == TArray<int32>({2, 4}));
	TestEqual(TEXT("first static-array restored"), F.Metadata->StaticValues[0], 1); TestEqual(TEXT("last static-array restored"), F.Metadata->StaticValues[1], 6);
	TestTrue(TEXT("soft null restored"), F.Metadata->SoftReference.IsNull());
	TestEqual(TEXT("no pre-edit"), F.Metadata->PreEditCount, 0); TestEqual(TEXT("no post-edit"), F.Metadata->PostEditCount, 0);
	TestEqual(TEXT("no reachable listener effect"), F.Metadata->ListenerEffect, 0); TestEqual(TEXT("no global property notifications"), Events.Changed, 0);
	TestEqual(TEXT("no Modify delegate"), Events.Modified, 0); TestFalse(TEXT("package stays clean"), F.Package->IsDirty());
	TestEqual(TEXT("owner footprint not enlarged"), F.Transaction->GetEnlistedFootprint().Num(), 1);
	TestTrue(TEXT("leaf never enlists another inverse"), F.Transaction->EnlistedRollbackActionComponentIds.IsEmpty());
	TestFalse(TEXT("inverse is one-shot"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentUserDataRoundtrip,
	"UEAssetDoc.Editor.SequenceOwnedFragment.UserDataRawRoundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentUserDataRoundtrip::RunTest(const FString&)
{
	FFixture F; if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	TStrongObjectPtr<UAssetDocumentEditorSequenceFragmentUserDataTest> Child(NewObject<UAssetDocumentEditorSequenceFragmentUserDataTest>(F.Parent.Get(), TEXT("UserData"), RF_Transactional));
	F.Parent->AddAssetUserData(Child.Get()); F.Context.Kind = EAssetDocumentEditorSequenceFragmentKind::AssetUserData;
	F.Package->SetDirtyFlag(false);
	auto P = Properties(); P.ObjectKeys[1] = TEXT("Label"); P.Elements[1] = String(TEXT("after"));
	TArray<FAssetDocumentRuntimeDiagnostic> D;
	auto Prepared = PrepareAssetDocumentEditorSequenceOwnedFragmentProperties(F.Context, *F.Lease, *F.Resolver, *Child.Get(), P, *F.Transaction, D);
	if (!TestTrue(TEXT("userdata preparation"), Prepared.IsValid())) return false;
	FNotifications Events(*Child.Get(), *F.Parent.Get());
	TestTrue(TEXT("userdata raw apply"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	TestEqual(TEXT("userdata scalar"), Child->First, 17); TestEqual(TEXT("userdata string"), Child->Label, FString(TEXT("after")));
	TestTrue(TEXT("userdata raw inverse"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid);
	TestEqual(TEXT("userdata scalar restored"), Child->First, 7); TestEqual(TEXT("userdata string restored"), Child->Label, FString(TEXT("before")));
	TestEqual(TEXT("no child edit"), Child->PostEditCount, 0); TestEqual(TEXT("no owner callback"), Child->OwnerEditCount, 0);
	TestEqual(TEXT("no global notification"), Events.Changed, 0); TestFalse(TEXT("no package dirty"), F.Package->IsDirty());
	TestTrue(TEXT("attachment unchanged"), F.Parent->GetAssetUserDataOfClass(Child->GetClass()) == Child.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentZeroWriteAdmission,
	"UEAssetDoc.Editor.SequenceOwnedFragment.LateInvalidFieldZeroWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentZeroWriteAdmission::RunTest(const FString&)
{
	FFixture F; if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	for (const FString Name : {FString(TEXT("Unknown")), FString(TEXT("ReadOnly")), FString(TEXT("InstanceOnly")), FString(TEXT("UnsupportedHardReference")), FString(TEXT("First"))})
	{
		auto P = Properties(); P.ObjectKeys[1] = Name;
		TArray<FAssetDocumentRuntimeDiagnostic> D; auto Prepared = F.Prepare(P, D);
		TestFalse(*FString::Printf(TEXT("late %s rejects"), *Name), Prepared.IsValid());
		TestEqual(TEXT("first never written"), F.Metadata->First, 3);
	}
	auto P = Properties(); P.Elements[1] = String(TEXT("bad integer"));
	TArray<FAssetDocumentRuntimeDiagnostic> D;
	TestFalse(TEXT("late decode rejects before first setter"), F.Prepare(P, D).IsValid());
	TestEqual(TEXT("first unchanged after decode failure"), F.Metadata->First, 3);
	auto Prepared = F.Prepare(Properties(), D);
	if (!TestTrue(TEXT("valid preparation"), Prepared.IsValid())) return false;
	F.Metadata->Second = 101;
	TestFalse(TEXT("late field preimage drift rejects entire raw apply"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	TestEqual(TEXT("first still unchanged"), F.Metadata->First, 3); TestEqual(TEXT("external field change preserved"), F.Metadata->Second, 101);
	TestTrue(TEXT("rollback can consume zero-write prepared participant"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid);
	TestEqual(TEXT("zero-write inverse does not undo unrelated external field change"), F.Metadata->Second, 101);
	TestFalse(TEXT("inverse consumption cannot revive forward apply"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	FFixture Static;
	auto StaticP = Properties(); StaticP.ObjectKeys[1] = TEXT("StaticValues");
	StaticP.Elements[1] = {};
	StaticP.Elements[1].Kind = EAssetDocumentRuntimeValueKind::Array;
	StaticP.Elements[1].Elements = {Integer(31), Integer(41)};
	auto StaticPrepared = Static.Prepare(StaticP, D);
	if (!TestTrue(TEXT("static-array preparation"), StaticPrepared.IsValid())) return false;
	Static.Metadata->StaticValues[1] = 123;
	TestFalse(TEXT("last static-array element drift rejects entire apply"), StaticPrepared->ApplyRaw(Static.Context, *Static.Lease, *Static.Resolver, *Static.Transaction).bValid);
	TestEqual(TEXT("static-array late drift prevents first scalar write"), Static.Metadata->First, 3);
	TestEqual(TEXT("external last static-array element preserved"), Static.Metadata->StaticValues[1], 123);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentIdentityAdmission,
	"UEAssetDoc.Editor.SequenceOwnedFragment.IdentityAndContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentIdentityAdmission::RunTest(const FString&)
{
	FFixture F; if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	TArray<FAssetDocumentRuntimeDiagnostic> D;
	auto Reject = [&](const FAssetDocumentEditorSequenceFragmentContext& C, const IAssetDocumentVerifiedHandleLease& L, UObject& Child)
	{ return !PrepareAssetDocumentEditorSequenceOwnedFragmentProperties(C, L, *F.Resolver, Child, Properties(), *F.Transaction, D).IsValid(); };
	auto Wrong = F.Context; Wrong.Mutation.RequestId = FGuid::NewGuid(); TestTrue(TEXT("wrong request"), Reject(Wrong, *F.Lease, *F.Metadata.Get()));
	Wrong = F.Context; ++Wrong.Mutation.RuntimeRevision; TestTrue(TEXT("stale revision"), Reject(Wrong, *F.Lease, *F.Metadata.Get()));
	Wrong = F.Context; Wrong.Mutation.TransactionId = FGuid::NewGuid(); TestTrue(TEXT("wrong transaction"), Reject(Wrong, *F.Lease, *F.Metadata.Get()));
	Wrong = F.Context; Wrong.Kind = EAssetDocumentEditorSequenceFragmentKind::AssetUserData; TestTrue(TEXT("wrong child class family"), Reject(Wrong, *F.Lease, *F.Metadata.Get()));
	Wrong = F.Context; Wrong.Phase = EAssetDocumentRuntimeRequestPhase::Verify; TestTrue(TEXT("verify cannot prepare writes"), Reject(Wrong, *F.Lease, *F.Metadata.Get()));
	auto ReadOnly = F.Acquire(*F.Parent.Get(), EAssetDocumentHandleAccess::ReadOnly); TestTrue(TEXT("readonly cannot prepare writes"), Reject(F.Context, *ReadOnly, *F.Metadata.Get()));
	const TArray<FString> Names = {TEXT("First"), TEXT("Second")};
	TestTrue(TEXT("readonly Verify read succeeds"), ReadAssetDocumentEditorSequenceOwnedFragmentProperties(Wrong, *ReadOnly, *F.Resolver, *F.Metadata.Get(), Names).bSucceeded);
	TStrongObjectPtr<UAnimMontage> OtherParent(NewObject<UAnimMontage>(F.Package.Get()));
	auto OtherLease = F.Acquire(*OtherParent.Get()); TestTrue(TEXT("wrong parent class"), Reject(F.Context, *OtherLease, *F.Metadata.Get()));
	TStrongObjectPtr<UObject> WrongClass(NewObject<UAssetDocumentEditorSequenceFragmentUserDataTest>(F.Parent.Get())); TestTrue(TEXT("wrong child class"), Reject(F.Context, *F.Lease, *WrongClass.Get()));
	FFixture Other; TestTrue(TEXT("wrong actual outer"), Reject(F.Context, *F.Lease, *Other.Metadata.Get()));
	auto Prepared = F.Prepare(Properties(), D); if (!TestTrue(TEXT("prepare"), Prepared.IsValid())) return false;
	Wrong = F.Context; Wrong.OperationId = FGuid::NewGuid();
	TestFalse(TEXT("participant belongs to exact operation"), Prepared->ApplyRaw(Wrong, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	F.Parent->RemoveMetaData(F.Metadata.Get());
	TestFalse(TEXT("detached participant rejected"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	TestFalse(TEXT("inverse requires owner to restore exact attachment first"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid);
	TestTrue(TEXT("detached new preparation rejected"), Reject(F.Context, *F.Lease, *F.Metadata.Get()));
	TestEqual(TEXT("all admission failures zero-write"), F.Metadata->First, 3);
	F.Parent->AddMetaData(F.Metadata.Get());
	F.Metadata->Rename(TEXT("ChangedIdentity"), F.Parent.Get(), REN_DontCreateRedirectors | REN_NonTransactional);
	TestFalse(TEXT("stale child path rejected"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	TestFalse(TEXT("inverse rejects native child path drift"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid);
	FFixture Terminal;
	auto TerminalPrepared = Terminal.Prepare(Properties(), D);
	if (!TestTrue(TEXT("terminal fixture preparation"), TerminalPrepared.IsValid())) return false;
	FAssetDocumentDurableCompletionReceipt Receipt;
	Terminal.Transaction->Commit(Receipt);
	TestFalse(TEXT("committed transaction cannot apply prepared raw fields"), TerminalPrepared->ApplyRaw(Terminal.Context, *Terminal.Lease, *Terminal.Resolver, *Terminal.Transaction).bValid);
	TestFalse(TEXT("committed transaction cannot consume native inverse"), TerminalPrepared->RestoreRaw(Terminal.Context, *Terminal.Transaction).bValid);
	TestEqual(TEXT("terminal transaction zero-write"), Terminal.Metadata->First, 3);
	FFixture Revoked;
	auto RevokedPrepared = Revoked.Prepare(Properties(), D);
	if (!TestTrue(TEXT("revoked fixture preparation"), RevokedPrepared.IsValid())) return false;
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = EAssetDocumentHandleInvalidationReason::Reinstance;
	Event.RequestId = Revoked.Binding.RequestId; Event.TransactionId = Revoked.Binding.TransactionId;
	Event.RuntimeRevision = Revoked.Binding.RuntimeRevision; Event.ProviderId = Revoked.Binding.ProviderId;
	TestEqual(TEXT("existing resolver really revokes parent handle"), Revoked.Resolver->Invalidate(Event).InvalidatedHandleCount, 1u);
	// The offered lease now points at a destroyed test payload. Only its immutable
	// handle may be used: touching GetPayloadChecked would be invalid native access.
	TestFalse(TEXT("revoked offered lease cannot apply"), RevokedPrepared->ApplyRaw(Revoked.Context, *Revoked.Lease, *Revoked.Resolver, *Revoked.Transaction).bValid);
	TestFalse(TEXT("revoked offered lease cannot capture"), Revoked.Prepare(Properties(), D).IsValid());
	TestFalse(TEXT("revoked offered lease cannot read"), ReadAssetDocumentEditorSequenceOwnedFragmentProperties(Revoked.Context, *Revoked.Lease, *Revoked.Resolver, *Revoked.Metadata.Get(), Names).bSucceeded);
	TestEqual(TEXT("revoked handle zero-write"), Revoked.Metadata->First, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentCoreRollback,
	"UEAssetDoc.Editor.SequenceOwnedFragment.CoreRollbackAfterHandleInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentCoreRollback::RunTest(const FString&)
{
	FFixture F;
	FRequestResolver Handles(F.Binding);
	auto Action = MakeShared<FNativeInverseAction>();
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions = {Action};
	FAssetDocumentRollbackActionComposition Composition(Handles, MoveTemp(Actions),
		[](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic&) {});
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = F.Binding.RequestId; Receipt.RuntimeRevision = F.Binding.RuntimeRevision;
	Receipt.CompleteFootprint = {{EAssetDocumentMutationFootprintKind::UObjectState,
		F.Parent->GetPathName(), TEXT("test.parent.inverse")}};
	Receipt.ReceiptHash = TEXT("test.sequence-owned-fragment.inverse"); Receipt.bSucceeded = true;
	FCancellation Cancellation;
	auto Transaction = Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
	if (!TestTrue(TEXT("real Core transaction begins through exported composition"), Transaction.IsValid())) return false;
	Handles.Binding.TransactionId = Transaction->GetTransactionId();
	Handles.Inner = MakeUnique<FAssetDocumentEditorProviderTestHandleResolver>(Handles.Binding);
	auto Context = F.Context; Context.Mutation = Transaction->GetMutationToken();
	FAssetDocumentHandleValidationContext Validation;
	Validation.RequestId = Context.Mutation.RequestId; Validation.TransactionId = Context.Mutation.TransactionId;
	Validation.RuntimeRevision = Context.Mutation.RuntimeRevision; Validation.ProviderId = TEXT("animation");
	Validation.Phase = Context.Phase; Validation.RequiredKind = EAssetDocumentHandleKind::Asset;
	Validation.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	auto Issued = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable,
		MakeAssetDocumentEditorAssetPayload(*F.Parent.Get()));
	if (!TestTrue(TEXT("request registry issues owned parent handle"), Issued.Handle.IsSet())) return false;
	auto Verified = Handles.AcquireVerifiedLease(*Issued.Handle, Validation);
	if (!TestTrue(TEXT("request registry verifies exact parent"), Verified.Lease.IsValid())) return false;
	TestTrue(TEXT("real parent footprint enlisted before leaf capture"), Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
	TArray<FAssetDocumentRuntimeDiagnostic> D;
	Action->Prepared = PrepareAssetDocumentEditorSequenceOwnedFragmentProperties(Context, *Verified.Lease, Handles,
		*F.Metadata.Get(), Properties(), *Transaction, D);
	if (!TestTrue(TEXT("typed native inverse retained by actual rollback action"), Action->Prepared.IsValid())) return false;
	Action->Context = Context; Action->Transaction = Transaction.Get(); Action->Resolver = &Handles;
	Action->Handle = *Issued.Handle; Action->Validation = Validation;
	FNotifications Events(*F.Metadata.Get(), *F.Parent.Get());
	TestTrue(TEXT("raw apply under real atomic transaction"), Action->Prepared->ApplyRaw(Context, *Verified.Lease, Handles, *Transaction).bValid);
	TestEqual(TEXT("forward field changed"), F.Metadata->First, 17);
	TestTrue(TEXT("pre-completion transaction can reach ReadyToCommit"), Transaction->MarkReadyToCommit().bValid);
	TestTrue(TEXT("real atomic rollback succeeds after registry invalidation"), Transaction->Rollback(TEXT("test.sequence-owned-fragment")).bValid);
	TestEqual(TEXT("real Core emits rollback invalidation once"), Handles.RollbackInvalidationCount, 1);
	TestEqual(TEXT("actual registry handle invalidated"), Handles.InvalidatedHandles, 1u);
	TestTrue(TEXT("native action observes revocation before inverse"), Action->bRevokedBeforeAction);
	TestEqual(TEXT("actual rollback action executes once"), Action->Calls, 1);
	TestEqual(TEXT("native inverse restored first"), F.Metadata->First, 3); TestEqual(TEXT("native inverse restored second"), F.Metadata->Second, 5);
	TestEqual(TEXT("inverse did not run property listeners"), Events.Changed, 0); TestEqual(TEXT("inverse did not Modify"), Events.Modified, 0);
	TestFalse(TEXT("inverse did not dirty package"), F.Package->IsDirty());
	TestFalse(TEXT("revoked forward capability stays revoked"), Handles.AcquireVerifiedLease(*Issued.Handle, Validation).Lease.IsValid());
	TestFalse(TEXT("terminal native inverse cannot repeat"), Action->Prepared->RestoreRaw(Context, *Transaction).bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentStrongLifetime,
	"UEAssetDoc.Editor.SequenceOwnedFragment.StrongNativeLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentStrongLifetime::RunTest(const FString&)
{
	FFixture F; if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	// FSharedStruct has native ARO but no reflected fields/serializer. Each payload
	// object is only visible to GC through the real native struct value, not a DTO.
	TArray<TWeakObjectPtr<UObject>> References;
	auto MakeReference = [&References]()
	{
		UObject* Object = NewObject<UAssetDocumentEditorSequenceFragmentUserDataTest>(GetTransientPackage());
		References.Add(Object);
		auto Value = FSharedStruct::Make<FAssetDocumentEditorSequenceFragmentReferenceTest>();
		Value.Get<FAssetDocumentEditorSequenceFragmentReferenceTest>().Object = Object;
		return Value;
	};
	F.Metadata->SharedValue = MakeReference();
	F.Metadata->NestedSharedValue.Value = MakeReference();
	F.Metadata->SharedValues.Add(MakeReference());
	F.Metadata->SharedMap.FindOrAdd(TEXT("held")).Value = MakeReference();
	{
		FAssetDocumentEditorSequenceFragmentSharedValueTest SetValue;
		SetValue.Value = MakeReference(); F.Metadata->SharedSet.Add(SetValue);
	}
	F.Metadata->SharedStaticValues[0] = MakeReference();
	F.Metadata->SharedStaticValues[1] = MakeReference();
	FAssetDocumentRuntimeValueDto EmptyStruct, EmptyArray, Nested, Static;
	EmptyStruct.Kind = EAssetDocumentRuntimeValueKind::Object;
	EmptyArray.Kind = EAssetDocumentRuntimeValueKind::Array;
	Nested.Kind = EAssetDocumentRuntimeValueKind::Object;
	Nested.ObjectKeys = {TEXT("Value")}; Nested.Elements = {EmptyStruct};
	Static.Kind = EAssetDocumentRuntimeValueKind::Array; Static.Elements = {EmptyStruct, EmptyStruct};
	auto P = Properties();
	P.ObjectKeys.Append({TEXT("SharedValue"), TEXT("NestedSharedValue"), TEXT("SharedValues"),
		TEXT("SharedMap"), TEXT("SharedSet"), TEXT("SharedStaticValues")});
	P.Elements.Append({EmptyStruct, Nested, EmptyArray, EmptyStruct, EmptyArray, Static});
	TArray<FAssetDocumentRuntimeDiagnostic> D; auto Prepared = F.Prepare(P, D);
	if (!TestTrue(TEXT("prepare"), Prepared.IsValid())) return false;
	FNotifications Events(*F.Metadata.Get(), *F.Parent.Get());
	if (!TestTrue(TEXT("raw apply clears child-owned references"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid)) return false;
	TestFalse(TEXT("top struct cleared"), F.Metadata->SharedValue.IsValid());
	TestFalse(TEXT("nested struct cleared"), F.Metadata->NestedSharedValue.Value.IsValid());
	TestTrue(TEXT("array cleared"), F.Metadata->SharedValues.IsEmpty());
	TestTrue(TEXT("map cleared"), F.Metadata->SharedMap.IsEmpty());
	TestTrue(TEXT("set cleared"), F.Metadata->SharedSet.IsEmpty());
	TestFalse(TEXT("static first cleared"), F.Metadata->SharedStaticValues[0].IsValid());
	TestFalse(TEXT("static last cleared"), F.Metadata->SharedStaticValues[1].IsValid());
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = EAssetDocumentHandleInvalidationReason::GarbageCollection;
	Event.RequestId = F.Binding.RequestId; Event.TransactionId = F.Binding.TransactionId;
	Event.RuntimeRevision = F.Binding.RuntimeRevision; Event.ProviderId = F.Binding.ProviderId;
	TestEqual(TEXT("forward handle revoked before GC"), F.Resolver->Invalidate(Event).InvalidatedHandleCount, 1u);
	TWeakObjectPtr<UAnimSequence> Parent(F.Parent.Get());
	TWeakObjectPtr<UAssetDocumentEditorSequenceFragmentMetadataTest> Child(F.Metadata.Get());
	F.Lease.Reset(); F.Metadata.Reset(); F.Parent.Reset(); F.Package.Reset();
	CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("prepared participant strongly retains exact parent"), Parent.IsValid());
	TestTrue(TEXT("prepared participant strongly retains child and captured field storage"), Child.IsValid());
	for (int32 I = 0; I < References.Num(); ++I)
		TestTrue(*FString::Printf(TEXT("captured ARO reference %d survives raw-clear GC"), I), References[I].IsValid());
	if (!Child.IsValid() || !TestTrue(TEXT("native inverse after GC needs no forward lease"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid)) return false;
	auto VerifyReference = [this, &References](const FSharedStruct& Value, int32 I, const TCHAR* Label)
	{
		const auto* Native = Value.GetPtr<FAssetDocumentEditorSequenceFragmentReferenceTest>();
		if (TestTrue(Label, Native != nullptr)) TestEqual(Label, Native->Object.Get(), References[I].Get());
	};
	VerifyReference(Child->SharedValue, 0, TEXT("top ARO reference restored"));
	VerifyReference(Child->NestedSharedValue.Value, 1, TEXT("nested ARO reference restored"));
	if (TestEqual(TEXT("array restored size"), Child->SharedValues.Num(), 1))
		VerifyReference(Child->SharedValues[0], 2, TEXT("array ARO reference restored"));
	const auto* MapValue = Child->SharedMap.Find(TEXT("held"));
	if (TestTrue(TEXT("map exact key restored"), MapValue != nullptr))
		VerifyReference(MapValue->Value, 3, TEXT("map ARO reference restored"));
	if (TestEqual(TEXT("set restored size"), Child->SharedSet.Num(), 1))
		for (const auto& Value : Child->SharedSet) VerifyReference(Value.Value, 4, TEXT("set ARO reference restored"));
	VerifyReference(Child->SharedStaticValues[0], 5, TEXT("static first ARO reference restored"));
	VerifyReference(Child->SharedStaticValues[1], 6, TEXT("static last ARO reference restored"));
	TestEqual(TEXT("GC roundtrip emits no property listener"), Events.Changed, 0);
	TestEqual(TEXT("GC roundtrip emits no Modify"), Events.Modified, 0);
	TestFalse(TEXT("GC roundtrip leaves package clean"), Child->GetOutermost()->IsDirty());
	Prepared.Reset();
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("participant releases parent when discarded"), Parent.IsValid());
	TestFalse(TEXT("participant releases child when discarded"), Child.IsValid());
	for (int32 I = 0; I < References.Num(); ++I)
		TestFalse(*FString::Printf(TEXT("discarded participant and child release ARO reference %d"), I), References[I].IsValid());
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentClassPlanAdmission,
	"UEAssetDoc.Editor.SequenceOwnedFragment.ClassPlanAdmissionBeforeCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentClassPlanAdmission::RunTest(const FString&)
{
	FFixture F(false); if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	FNotifications Events(*F.Metadata.Get(), *F.Parent.Get());
	const int32 Count = F.Parent->GetMetaData().Num();
	TArray<FAssetDocumentRuntimeDiagnostic> D;
	auto P = Properties(); P.Elements[1] = String(TEXT("not an integer"));
	TestFalse(TEXT("late malformed field rejected before create"), F.ClassPlan(P, D).IsValid());
	for (const auto* Field : {TEXT("Unknown"), TEXT("ReadOnly"), TEXT("UnsupportedHardReference")})
	{
		P = Properties(); P.ObjectKeys.Add(Field); P.Elements.Add(Integer(1));
		TestFalse(TEXT("unknown/readonly/unsupported field rejected before create"), F.ClassPlan(P, D).IsValid());
	}
	TestFalse(TEXT("wrong family class rejected"), PrepareAssetDocumentEditorSequenceOwnedFragmentClassPlan(
		F.Context, *F.Lease, *F.Resolver, *UAssetDocumentEditorSequenceFragmentUserDataTest::StaticClass(),
		Properties(), *F.Transaction, D).IsValid());
	TestFalse(TEXT("abstract stock class rejected"), PrepareAssetDocumentEditorSequenceOwnedFragmentClassPlan(
		F.Context, *F.Lease, *F.Resolver, *UAnimMetaData::StaticClass(), Properties(), *F.Transaction, D).IsValid());
	TestEqual(TEXT("preflight never creates or attaches"), F.Parent->GetMetaData().Num(), Count);
	TestEqual(TEXT("preflight never writes existing child"), F.Metadata->First, 3);
	TestEqual(TEXT("preflight never enlists"), F.Transaction->GetEnlistedFootprint().Num(), 0);
	TestTrue(TEXT("transaction stays Live"), F.Transaction->GetState() == EAssetDocumentAtomicState::Live);
	TestEqual(TEXT("preflight no notifying callback"), Events.Changed, 0);
	TestEqual(TEXT("preflight no Modify"), Events.Modified, 0);
	TestFalse(TEXT("preflight no dirty"), F.Package->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentAttachedLivePrepare,
	"UEAssetDoc.Editor.SequenceOwnedFragment.AttachedLivePrepareBeforeEnlist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentAttachedLivePrepare::RunTest(const FString&)
{
	FFixture F(false); if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	FNotifications Events(*F.Metadata.Get(), *F.Parent.Get());
	TArray<FAssetDocumentRuntimeDiagnostic> D; auto Prepared = F.Prepare(Properties(), D);
	if (!TestTrue(TEXT("existing-child capture before enlist in Live"), Prepared.IsValid())) return false;
	TestEqual(TEXT("capture no enlist"), F.Transaction->GetEnlistedFootprint().Num(), 0);
	TestEqual(TEXT("capture no setter"), F.Metadata->First, 3);
	TestFalse(TEXT("Live forward setter rejected"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	TestEqual(TEXT("Live rejection no setter"), F.Metadata->First, 3);
	const TArray<FAssetDocumentMutationFootprint> Footprint = {{EAssetDocumentMutationFootprintKind::UObjectState,
		F.Parent->GetPathName(), TEXT("test.parent.inverse")}};
	TestTrue(TEXT("owner enlists nonempty parent footprint"), F.Transaction->EnlistFootprint(Footprint).bValid);
	TestTrue(TEXT("same captured participant applies only after Mutating"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	TestEqual(TEXT("desired field applied"), F.Metadata->First, 17);
	TestTrue(TEXT("same native inverse remains valid"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid);
	TestEqual(TEXT("captured pre-enlist field restored"), F.Metadata->First, 3);
	TestEqual(TEXT("capture/raw/inverse no listeners"), Events.Changed, 0);
	TestEqual(TEXT("capture/raw/inverse no Modify"), Events.Modified, 0);
	TestFalse(TEXT("capture/raw/inverse no dirty"), F.Package->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentClassPlanIdentity,
	"UEAssetDoc.Editor.SequenceOwnedFragment.ClassPlanIdentityAndSingleBind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentClassPlanIdentity::RunTest(const FString&)
{
	FFixture F; if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	TArray<FAssetDocumentRuntimeDiagnostic> D; auto Plan = F.ClassPlan(Properties(), D);
	if (!TestTrue(TEXT("exact class plan"), Plan.IsValid())) return false;
	auto Wrong = F.Context; Wrong.OperationId = FGuid::NewGuid();
	TestFalse(TEXT("cross operation fresh validation rejected"), Plan->ValidateBeforeCreate(Wrong, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	Wrong = F.Context; ++Wrong.Mutation.RuntimeRevision;
	TestFalse(TEXT("stale revision rejected"), Plan->ValidateBeforeCreate(Wrong, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	Wrong = F.Context; Wrong.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	TestFalse(TEXT("wrong phase rejected"), Plan->ValidateBeforeCreate(Wrong, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	FAssetDocumentEditorProviderTestTransaction Other(F.Binding);
	const TArray<FAssetDocumentMutationFootprint> Footprint = {{EAssetDocumentMutationFootprintKind::UObjectState,
		F.Parent->GetPathName(), TEXT("test.parent.inverse")}};
	Other.EnlistFootprint(Footprint);
	TestFalse(TEXT("different transaction object with same tuple rejected"), Plan->ValidateBeforeCreate(F.Context, *F.Lease, *F.Resolver, Other).bValid);
	UClass* Class = UAssetDocumentEditorSequenceFragmentMetadataTest::StaticClass();
	Class->ClassFlags |= CLASS_NewerVersionExists;
	TestFalse(TEXT("stale class flags rejected"), Plan->ValidateBeforeCreate(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	Class->ClassFlags &= ~CLASS_NewerVersionExists;
	FProperty* First = FindFProperty<FProperty>(Class, TEXT("First"));
	First->SetPropertyFlags(CPF_EditConst);
	TestFalse(TEXT("changed admitted field access rejected"), Plan->ValidateBeforeCreate(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	First->ClearPropertyFlags(CPF_EditConst);
	TestTrue(TEXT("unchanged exact context stays valid"), Plan->ValidateBeforeCreate(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	UObject* WrongClass = NewObject<UAssetDocumentEditorSequenceFragmentUserDataTest>(F.Parent.Get());
	TestFalse(TEXT("wrong exact child class rejected"), Plan->BindOnce(F.Context, *F.Lease, *F.Resolver, *WrongClass, *F.Transaction, D).IsValid());
	TestFalse(TEXT("failed attempt consumed"), Plan->BindOnce(F.Context, *F.Lease, *F.Resolver, *F.Metadata.Get(), *F.Transaction, D).IsValid());
	TestFalse(TEXT("consumed plan cannot authorize creation"), Plan->ValidateBeforeCreate(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	Plan = F.ClassPlan(Properties(), D);
	UObject* Detached = NewObject<UAssetDocumentEditorSequenceFragmentMetadataTest>(F.Parent.Get());
	TestFalse(TEXT("direct outer alone is not attachment"), Plan->BindOnce(F.Context, *F.Lease, *F.Resolver, *Detached, *F.Transaction, D).IsValid());
	Plan = F.ClassPlan(Properties(), D);
	UObject* WrongOuter = NewObject<UAssetDocumentEditorSequenceFragmentMetadataTest>(GetTransientPackage());
	TestFalse(TEXT("wrong native outer rejected"), Plan->BindOnce(F.Context, *F.Lease, *F.Resolver, *WrongOuter, *F.Transaction, D).IsValid());
	// A class projection cannot predict the actual EditInlineNew template access.
	FAssetDocumentRuntimeValueDto Instance; Instance.Kind = EAssetDocumentRuntimeValueKind::Object;
	Instance.ObjectKeys = {TEXT("InstanceOnly")}; Instance.Elements = {Integer(23)};
	Plan = F.ClassPlan(Instance, D);
	if (TestTrue(TEXT("class-only admission succeeds without CDO"), Plan.IsValid()))
		TestFalse(TEXT("actual template instance access checked at bind"), Plan->BindOnce(F.Context, *F.Lease, *F.Resolver, *F.Metadata.Get(), *F.Transaction, D).IsValid());
	FFixture Live(false); Plan = Live.ClassPlan(Properties(), D);
	if (TestTrue(TEXT("Live pre-create plan"), Plan.IsValid()))
	{
		TestTrue(TEXT("Live fresh validation"), Plan->ValidateBeforeCreate(Live.Context, *Live.Lease, *Live.Resolver, *Live.Transaction).bValid);
		TestFalse(TEXT("bind requires owner enlistment"), Plan->BindOnce(Live.Context, *Live.Lease, *Live.Resolver, *Live.Metadata.Get(), *Live.Transaction, D).IsValid());
		Live.Transaction->EnlistFootprint(Footprint);
		TestFalse(TEXT("early failed bind cannot retry after enlistment"), Plan->BindOnce(Live.Context, *Live.Lease, *Live.Resolver, *Live.Metadata.Get(), *Live.Transaction, D).IsValid());
	}
	FFixture Revoked(false); Plan = Revoked.ClassPlan(Properties(), D);
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = EAssetDocumentHandleInvalidationReason::GarbageCollection;
	Event.RequestId = Revoked.Binding.RequestId; Event.TransactionId = Revoked.Binding.TransactionId;
	Event.RuntimeRevision = Revoked.Binding.RuntimeRevision; Event.ProviderId = Revoked.Binding.ProviderId;
	Revoked.Resolver->Invalidate(Event);
	if (TestTrue(TEXT("plan retained before revocation"), Plan.IsValid()))
		TestFalse(TEXT("offered lease is reacquired before stale payload access"), Plan->ValidateBeforeCreate(Revoked.Context, *Revoked.Lease, *Revoked.Resolver, *Revoked.Transaction).bValid);
	FFixture UserData;
	UserData.Context.Kind = EAssetDocumentEditorSequenceFragmentKind::AssetUserData;
	TStrongObjectPtr<UAssetDocumentEditorSequenceFragmentUserDataTest> Data(NewObject<UAssetDocumentEditorSequenceFragmentUserDataTest>(UserData.Parent.Get()));
	UserData.Parent->AddAssetUserData(Data.Get());
	FAssetDocumentRuntimeValueDto DataProperties; DataProperties.Kind = EAssetDocumentRuntimeValueKind::Object;
	DataProperties.ObjectKeys = {TEXT("First")}; DataProperties.Elements = {Integer(37)};
	Plan = PrepareAssetDocumentEditorSequenceOwnedFragmentClassPlan(UserData.Context, *UserData.Lease, *UserData.Resolver,
		*UAssetDocumentEditorSequenceFragmentUserDataTest::StaticClass(), DataProperties, *UserData.Transaction, D);
	if (TestTrue(TEXT("fixed UserData class plan accepted"), Plan.IsValid()))
	{
		auto Bound = Plan->BindOnce(UserData.Context, *UserData.Lease, *UserData.Resolver, *Data.Get(), *UserData.Transaction, D);
		if (TestTrue(TEXT("exact UserData attachment bound"), Bound.IsValid()))
		{
			TestTrue(TEXT("UserData plan raw apply"), Bound->ApplyRaw(UserData.Context, *UserData.Lease, *UserData.Resolver, *UserData.Transaction).bValid);
			TestEqual(TEXT("UserData decoded value"), Data->First, 37);
			TestTrue(TEXT("UserData plan inverse"), Bound->RestoreRaw(UserData.Context, *UserData.Transaction).bValid);
			TestEqual(TEXT("UserData exact preimage"), Data->First, 7);
		}
	}
	TestEqual(TEXT("all rejections leave child unchanged"), F.Metadata->First, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentClassPlanCreateRoundtrip,
	"UEAssetDoc.Editor.SequenceOwnedFragment.ClassPlanPublicCreateRoundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentClassPlanCreateRoundtrip::RunTest(const FString&)
{
	FFixture F(false); FRequestResolver Handles(F.Binding);
	auto Action = MakeShared<FNativeInverseAction>();
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions = {Action};
	FAssetDocumentRollbackActionComposition Composition(Handles, MoveTemp(Actions),
		[](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic&) {});
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = F.Binding.RequestId; Receipt.RuntimeRevision = F.Binding.RuntimeRevision;
	Receipt.CompleteFootprint = {{EAssetDocumentMutationFootprintKind::UObjectState, F.Parent->GetPathName(), TEXT("test.parent.inverse")}};
	Receipt.ReceiptHash = TEXT("test.sequence-fragment.class-plan"); Receipt.bSucceeded = true;
	FCancellation Cancellation; auto Transaction = Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
	if (!TestTrue(TEXT("real Live Core transaction"), Transaction.IsValid())) return false;
	Handles.Binding.TransactionId = Transaction->GetTransactionId();
	Handles.Inner = MakeUnique<FAssetDocumentEditorProviderTestHandleResolver>(Handles.Binding);
	auto Context = F.Context; Context.Mutation = Transaction->GetMutationToken();
	FAssetDocumentHandleValidationContext Validation;
	Validation.RequestId = Context.Mutation.RequestId; Validation.TransactionId = Context.Mutation.TransactionId;
	Validation.RuntimeRevision = Context.Mutation.RuntimeRevision; Validation.ProviderId = TEXT("animation");
	Validation.Phase = Context.Phase; Validation.RequiredKind = EAssetDocumentHandleKind::Asset; Validation.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	auto Issued = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, MakeAssetDocumentEditorAssetPayload(*F.Parent.Get()));
	if (!TestTrue(TEXT("parent handle"), Issued.Handle.IsSet())) return false;
	auto Verified = Handles.AcquireVerifiedLease(*Issued.Handle, Validation);
	if (!TestTrue(TEXT("parent lease"), Verified.Lease.IsValid())) return false;
	TArray<FAssetDocumentRuntimeDiagnostic> D; auto P = Properties();
	const int32 Before = F.Parent->GetMetaData().Num();
	auto Plan = PrepareAssetDocumentEditorSequenceOwnedFragmentClassPlan(Context, *Verified.Lease, Handles,
		*UAssetDocumentEditorSequenceFragmentMetadataTest::StaticClass(), P, *Transaction, D);
	if (!TestTrue(TEXT("desired decoded while Live before creation/enlistment"), Plan.IsValid())) return false;
	TestEqual(TEXT("plan does not attach"), F.Parent->GetMetaData().Num(), Before);
	TestTrue(TEXT("plan does not enlist"), Transaction->GetState() == EAssetDocumentAtomicState::Live);
	TestTrue(TEXT("fresh read-only check immediately before owner creation"), Plan->ValidateBeforeCreate(Context, *Verified.Lease, Handles, *Transaction).bValid);
	P.Elements[0] = String(TEXT("would fail if Bind decoded again"));
	TestTrue(TEXT("owner enlists before fixture creation"), Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
	// Test-owned transient construction + stock exported attachment. The leaf
	// never performs this construction and never claims a creation receipt.
	TStrongObjectPtr<UAssetDocumentEditorSequenceFragmentMetadataTest> Child(NewObject<UAssetDocumentEditorSequenceFragmentMetadataTest>(F.Parent.Get(), NAME_None, RF_Transactional));
	F.Parent->AddMetaData(Child.Get());
	FNotifications Events(*Child.Get(), *F.Parent.Get()); F.Package->SetDirtyFlag(false);
	Action->Prepared = Plan->BindOnce(Context, *Verified.Lease, Handles, *Child.Get(), *Transaction, D);
	if (!TestTrue(TEXT("exact new attached child binds decoded plan"), Action->Prepared.IsValid())) return false;
	TestFalse(TEXT("successful bind consumes plan"), Plan->BindOnce(Context, *Verified.Lease, Handles, *Child.Get(), *Transaction, D).IsValid());
	TestFalse(TEXT("successful bind cannot revalidate creation"), Plan->ValidateBeforeCreate(Context, *Verified.Lease, Handles, *Transaction).bValid);
	Action->Context = Context; Action->Transaction = Transaction.Get(); Action->Resolver = &Handles;
	Action->Handle = *Issued.Handle; Action->Validation = Validation;
	TestTrue(TEXT("bound raw participant applies"), Action->Prepared->ApplyRaw(Context, *Verified.Lease, Handles, *Transaction).bValid);
	TestEqual(TEXT("Bind did not re-decode mutated DTO"), Child->First, 17);
	TestTrue(TEXT("real invalidate-before-action rollback"), Transaction->Rollback(TEXT("test.class-plan")).bValid);
	TestTrue(TEXT("native inverse executes after revocation"), Action->bRevokedBeforeAction);
	TestEqual(TEXT("inverse restores captured new-child default"), Child->First, 3);
	TestEqual(TEXT("inverse restores all captured fields"), Child->Second, 5);
	TestEqual(TEXT("leaf does not remove owner-created attachment"), F.Parent->GetMetaData().Num(), Before + 1);
	TestEqual(TEXT("raw plan participant no listeners"), Events.Changed, 0);
	TestEqual(TEXT("raw plan participant no Modify"), Events.Modified, 0);
	TestFalse(TEXT("raw plan participant no dirty"), F.Package->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFragmentClassPlanStrongDesired,
	"UEAssetDoc.Editor.SequenceOwnedFragment.ClassPlanStrongDesiredLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFragmentClassPlanStrongDesired::RunTest(const FString&)
{
	FFixture F(false); if (!TestTrue(TEXT("parent lease"), F.Lease.IsValid())) return false;
	FAssetDocumentRuntimeValueDto P, Empty;
	P.Kind = Empty.Kind = EAssetDocumentRuntimeValueKind::Object;
	P.ObjectKeys = {TEXT("NativeDesired")}; P.Elements = {Empty};
	TWeakObjectPtr<UObject> Reference(NewObject<UAssetDocumentEditorSequenceFragmentUserDataTest>(GetTransientPackage()));
	FAssetDocumentEditorSequenceFragmentDesiredReferenceTest::SetDefaultReference(Reference.Get());
	TArray<FAssetDocumentRuntimeDiagnostic> D; auto Plan = F.ClassPlan(P, D);
	FAssetDocumentEditorSequenceFragmentDesiredReferenceTest::SetDefaultReference(nullptr);
	if (!TestTrue(TEXT("native desired ARO value decoded before child creation"), Plan.IsValid())) return false;
	TestNull(TEXT("existing child does not own desired reference"), F.Metadata->NativeDesired.Reference.Get());
	CollectGarbage(RF_NoFlags);
	if (!TestTrue(TEXT("plan desired-only ARO reference survives GC"), Reference.IsValid())) return false;
	TestTrue(TEXT("fresh desired reference validation after GC"), Plan->ValidateBeforeCreate(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	const TArray<FAssetDocumentMutationFootprint> Footprint = {{EAssetDocumentMutationFootprintKind::UObjectState, F.Parent->GetPathName(), TEXT("test.parent.inverse")}};
	F.Transaction->EnlistFootprint(Footprint);
	auto Prepared = Plan->BindOnce(F.Context, *F.Lease, *F.Resolver, *F.Metadata.Get(), *F.Transaction, D);
	if (!TestTrue(TEXT("already-decoded native desired transferred at bind"), Prepared.IsValid())) return false;
	Plan.Reset(); CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("returned participant retains transferred desired"), Reference.IsValid());
	TestTrue(TEXT("native desired raw apply"), Prepared->ApplyRaw(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
	TestEqual(TEXT("actual decoded native reference applied"), F.Metadata->NativeDesired.Reference.Get(), Reference.Get());
	TestTrue(TEXT("native desired raw inverse"), Prepared->RestoreRaw(F.Context, *F.Transaction).bValid);
	TestNull(TEXT("native before default restored"), F.Metadata->NativeDesired.Reference.Get());
	Prepared.Reset(); CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("discard/inverse releases desired-only reference"), Reference.IsValid());
	UObject* Invalid = NewObject<UAssetDocumentEditorSequenceFragmentUserDataTest>(GetTransientPackage());
	FAssetDocumentEditorSequenceFragmentDesiredReferenceTest::SetDefaultReference(Invalid);
	Plan = F.ClassPlan(P, D); FAssetDocumentEditorSequenceFragmentDesiredReferenceTest::SetDefaultReference(nullptr);
	if (TestTrue(TEXT("second desired plan"), Plan.IsValid()))
	{
		Invalid->MarkAsGarbage();
		TestFalse(TEXT("stale native desired reference rejected before create"), Plan->ValidateBeforeCreate(F.Context, *F.Lease, *F.Resolver, *F.Transaction).bValid);
		TestFalse(TEXT("stale native desired reference rejected at bind"), Plan->BindOnce(F.Context, *F.Lease, *F.Resolver, *F.Metadata.Get(), *F.Transaction, D).IsValid());
	}
	return true;
}
} // namespace AssetDocumentEditorSequenceOwnedFragmentTests
#endif
