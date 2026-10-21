// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Sequence/AssetDocumentAnimationSequenceOwnedFragmentLifecycle.h"
#include "AssetDocumentAnimationSequenceOwnedFragmentLifecycleTestTypes.h"
#include "AssetDocumentAnimationSequenceOwnedFragmentLifecycleTestHarness.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentRollbackAction.h"
#include "Animation/AnimSequence.h"
#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectArray.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentSequenceOwnedFragmentOrderTests
{
using namespace AssetDocumentSequenceOwnedFragmentLifecycle;
using EKind = EAssetDocumentEditorSequenceFragmentKind;
FOrderedFragment Existing(UObject* Object)
{
	FOrderedFragment E; E.Existing = Object; E.Properties.Kind = EAssetDocumentRuntimeValueKind::Object; return E;
}
FOrderedFragment Create(UClass* Class)
{
	FOrderedFragment E; E.CreateClass = Class; E.Properties.Kind = EAssetDocumentRuntimeValueKind::Object; return E;
}
struct FFixture
{
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UAnimSequence> Parent;
	TStrongObjectPtr<UAssetDocumentSequenceLifecycleMetadataTest> ManagedMetadata, UnmanagedMetadata;
	TStrongObjectPtr<UAssetDocumentSequenceLifecycleUserDataTest> ManagedData;
	TStrongObjectPtr<UAssetDocumentSequenceLifecycleOtherUserDataTest> UnmanagedData;
	FFixture()
	{
		Package.Reset(CreatePackage(*(TEXT("/Temp/SequenceLifecycleOrder_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		Parent.Reset(NewObject<UAnimSequence>(Package.Get(), TEXT("Sequence"), RF_Public | RF_Transactional));
		// Raw construction belongs only to this newly created fixture. Production
		// will use the stock exported class helper after full decode and enlistment.
		ManagedMetadata.Reset(NewObject<UAssetDocumentSequenceLifecycleMetadataTest>(Parent.Get(), TEXT("ManagedMetadata")));
		UnmanagedMetadata.Reset(NewObject<UAssetDocumentSequenceLifecycleMetadataTest>(Parent.Get(), TEXT("UnmanagedMetadata")));
		ManagedMetadata->First = 17; UnmanagedMetadata->First = 29; UnmanagedMetadata->Values = {2, 5, 9};
		Parent->AddMetaData(ManagedMetadata.Get()); Parent->AddMetaData(UnmanagedMetadata.Get());
		ManagedData.Reset(NewObject<UAssetDocumentSequenceLifecycleUserDataTest>(Parent.Get(), TEXT("ManagedData")));
		UnmanagedData.Reset(NewObject<UAssetDocumentSequenceLifecycleOtherUserDataTest>(Parent.Get(), TEXT("UnmanagedData")));
		Parent->AddAssetUserData(ManagedData.Get()); Parent->AddAssetUserData(UnmanagedData.Get()); Package->SetDirtyFlag(false);
	}
	FReplacement Metadata() const
	{
		FReplacement R; R.Managed = {ManagedMetadata.Get()};
		R.Order = {Existing(ManagedMetadata.Get()), Existing(UnmanagedMetadata.Get())}; return R;
	}
	FReplacement UserData() const
	{
		FReplacement R; R.Kind = EKind::AssetUserData; R.Managed = {ManagedData.Get()};
		R.Order = {Existing(ManagedData.Get()), Existing(UnmanagedData.Get())}; return R;
	}
	void Unchanged(FAutomationTestBase& T) const
	{
		T.TestTrue(TEXT("readonly feasibility retains complete metadata order"),
			Parent->GetMetaData() == TArray<UAnimMetaData*>{ManagedMetadata.Get(), UnmanagedMetadata.Get()});
		T.TestTrue(TEXT("readonly feasibility retains complete user-data order"),
			*Parent->GetAssetUserDataArray() == TArray<UAssetUserData*>{ManagedData.Get(), UnmanagedData.Get()});
		T.TestEqual(TEXT("managed metadata payload untouched"), ManagedMetadata->First, 17);
		T.TestEqual(TEXT("unmanaged metadata payload untouched"), UnmanagedMetadata->First, 29);
		T.TestTrue(TEXT("unmanaged metadata container untouched"), UnmanagedMetadata->Values == TArray<int32>{2, 5, 9});
		T.TestFalse(TEXT("readonly feasibility leaves package clean"), Package->IsDirty());
	}
};

class FParentInverse final : public IAssetDocumentRollbackAction
{
public:
	FAssetDocumentEditorSequenceFragmentContext Context;
	TUniquePtr<FParentPreimage> Preimage;
	TArray<UObject*> ExactOwnedCurrent;
	IAssetDocumentAtomicTransaction* Transaction = nullptr;
	FResolver* Resolver = nullptr;
	FAssetDocumentOpaqueHandle Handle;
	FAssetDocumentHandleValidationContext Validation;
	bool bRevokedBeforeInverse = false;
	FStringView GetRollbackComponentId() const override { return TEXT("test.sequence-owned-fragment.parent.inverse"); }
	FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		bRevokedBeforeInverse = !Resolver->AcquireVerifiedLease(Handle, Validation).Lease.IsValid();
		return Preimage->RestoreAttachments(Context, *Transaction, ExactOwnedCurrent);
	}
};
class FCreationObserver final : public FUObjectArray::FUObjectCreateListener
{
	const UObjectBase* Parent;
	UClass* Class;
	bool bRegistered = false;
public:
	int32 Count = 0;
	FCreationObserver(UObject& InParent, UClass& InClass) : Parent(&InParent), Class(&InClass)
	{ GUObjectArray.AddUObjectCreateListener(this); bRegistered = true; }
	~FCreationObserver() { if (bRegistered) GUObjectArray.RemoveUObjectCreateListener(this); }
	void NotifyUObjectCreated(const UObjectBase* Object, int32) override
	{ if (Object->GetOuter() == Parent && Object->GetClass() == Class) ++Count; }
	void OnUObjectArrayShutdown() override
	{ GUObjectArray.RemoveUObjectCreateListener(this); bRegistered = false; }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceOwnedFragmentAttachmentOrder,
	"UEAssetDoc.AnimationProvider.SequenceOwnedFragmentLifecycle.AttachmentOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceOwnedFragmentAttachmentOrder::RunTest(const FString&)
{
	using namespace AssetDocumentSequenceOwnedFragmentOrderTests;
	FFixture F;
	TestTrue(TEXT("exact existing metadata order is representable"), ValidateAttachmentOrder(*F.Parent.Get(), F.Metadata()).bValid);
	TestTrue(TEXT("exact existing user-data order is representable"), ValidateAttachmentOrder(*F.Parent.Get(), F.UserData()).bValid);
	{
		auto R = F.Metadata(); R.Order.RemoveAt(0);
		TestTrue(TEXT("managed omission preserves unmanaged metadata"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid);
		R.Order.Insert(Create(UAssetDocumentSequenceLifecycleMetadataTest::StaticClass()), 0);
		TestTrue(TEXT("metadata pointer helpers permit same class with generated stock name"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid);
	}
	{
		auto R = F.UserData(); R.Order.RemoveAt(0);
		TestTrue(TEXT("managed omission preserves unrelated user-data class"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid);
		R.Order.Insert(Create(UAssetDocumentSequenceLifecycleDerivedUserDataTest::StaticClass()), 0);
		TestTrue(TEXT("replacement may change a deleted managed class while final selectors stay lossless"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid);
	}
	for (int32 Case = 0; Case < 10; ++Case)
	{
		auto R = F.Metadata();
		if (Case == 0) R.Managed.Add(F.ManagedMetadata.Get());
		if (Case == 1) R.Managed.Add(F.ManagedData.Get());
		if (Case == 2) R.Order.Add(Existing(F.ManagedMetadata.Get()));
		if (Case == 3) R.Order.RemoveAt(1);
		if (Case == 4) R.Order[1].Name = TEXT("different_name");
		if (Case == 5) R.Order[0].CreateClass = UAssetDocumentSequenceLifecycleMetadataTest::StaticClass();
		if (Case == 6) R.Order[0] = Create(UAnimMetaData::StaticClass());
		if (Case == 7) R.Order[0] = Create(UAssetDocumentSequenceLifecycleUserDataTest::StaticClass());
		if (Case == 8) { R.Order[0] = Create(UAssetDocumentSequenceLifecycleMetadataTest::StaticClass()); R.Order[0].Name = TEXT("Explicit"); }
		if (Case == 9)
		{
			R.Order[1].Properties.ObjectKeys = {TEXT("First")};
			FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Integer; V.IntegerValue = 101; R.Order[1].Properties.Elements = {V};
		}
		TestFalse(FString::Printf(TEXT("identity/name/unmanaged violation rejects without writes (case %d)"), Case),
			ValidateAttachmentOrder(*F.Parent.Get(), R).bValid); F.Unchanged(*this);
	}
	for (UClass* C : {UAssetDocumentSequenceLifecycleUserDataTest::StaticClass(), UAssetDocumentSequenceLifecycleOtherUserDataTest::StaticClass()})
	{
		auto R = F.UserData(); R.Order.Add(Create(C));
		TestFalse(TEXT("same-class duplicate rejects before allocation"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid); F.Unchanged(*this);
	}
	{
		auto R = F.UserData(); R.Order.Add(Create(UAssetDocumentSequenceLifecycleDerivedUserDataTest::StaticClass()));
		TestTrue(TEXT("base-before-derived order preserves the exact public Add prefix"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid);
		R.Order.Swap(0, 2);
		TestFalse(TEXT("derived-before-base order would evict an earlier object"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid); F.Unchanged(*this);
	}
	{
		TStrongObjectPtr<UAssetDocumentSequenceLifecycleDerivedUserDataTest> Derived(
			NewObject<UAssetDocumentSequenceLifecycleDerivedUserDataTest>(F.Parent.Get(), TEXT("InheritedCollision")));
		F.Parent->AddAssetUserData(Derived.Get()); auto R = F.UserData(); R.Managed.Add(Derived.Get());
		TestTrue(TEXT("an original base-before-derived pair has a lossless public inverse"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid);
		TestEqual(TEXT("original class proof writes no array element"), F.Parent->GetAssetUserDataArray()->Num(), 3);
		F.Parent->RemoveUserDataOfClass(Derived->GetClass());
	}
	{
		// Metadata permits multiple exact named instances. Their relative unmanaged
		// order remains fixed even when the caller chooses other managed positions.
		TStrongObjectPtr<UAssetDocumentSequenceLifecycleMetadataTest> Second(
			NewObject<UAssetDocumentSequenceLifecycleMetadataTest>(F.Parent.Get(), TEXT("SecondUnmanaged")));
		F.Parent->AddMetaData(Second.Get()); auto R = F.Metadata(); R.Order.Add(Existing(Second.Get()));
		R.Order.Swap(1, 2);
		TestFalse(TEXT("unmanaged relative order cannot be changed by replacement"), ValidateAttachmentOrder(*F.Parent.Get(), R).bValid);
		TestEqual(TEXT("refusal retains the actual third attachment"), F.Parent->GetMetaData()[2], static_cast<UAnimMetaData*>(Second.Get()));
		F.Parent->RemoveMetaData(Second.Get());
	}
	F.Unchanged(*this); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceOwnedFragmentParentCoreRollback,
	"UEAssetDoc.AnimationProvider.SequenceOwnedFragmentLifecycle.ParentPreimageCoreRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceOwnedFragmentParentCoreRollback::RunTest(const FString&)
{
	using namespace AssetDocumentSequenceOwnedFragmentOrderTests;
	for (const EKind Kind : {EKind::Metadata, EKind::AssetUserData})
	{
		FFixture F; FResolver Resolver; auto Action = MakeShared<FParentInverse>();
		FAssetDocumentRollbackActionComposition Composition(Resolver, {Action}, [](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic&) {});
		FAssetDocumentPreflightReceipt Receipt; Receipt.RequestId = FGuid::NewGuid(); Receipt.RuntimeRevision = 19;
		Receipt.CompleteFootprint = {
			{EAssetDocumentMutationFootprintKind::UObjectState, F.Parent->GetPathName(), FString(Action->GetRollbackComponentId())},
			{EAssetDocumentMutationFootprintKind::Package, F.Package->GetName(), FString(Action->GetRollbackComponentId())}};
		Receipt.ReceiptHash = TEXT("test.sequence-owned-fragment.parent"); Receipt.bSucceeded = true;
		FCancellation Cancellation; auto Transaction = Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
		if (!TestTrue(TEXT("real Core transaction begins before native lifecycle footprint"), Transaction.IsValid())) return false;
		Resolver.Token = Transaction->GetMutationToken(); auto Handle = Resolver.ResolveParent(*F.Parent.Get(), *Transaction);
		if (!TestTrue(TEXT("actual Editor resolves the native parent Asset payload"), Handle.IsSet())) return false;
		FAssetDocumentHandleValidationContext V; V.RequestId = Resolver.Token.RequestId; V.TransactionId = Resolver.Token.TransactionId;
		V.RuntimeRevision = Resolver.Token.RuntimeRevision; V.ProviderId = TEXT("animation"); V.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
		V.RequiredKind = EAssetDocumentHandleKind::Asset; V.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
		auto Verified = Resolver.AcquireVerifiedLease(Handle.GetValue(), V);
		if (!TestTrue(TEXT("immutable Editor parent payload verifies in request registry"), Verified.Lease.IsValid())) return false;
		FAssetDocumentEditorSequenceFragmentContext C; C.OperationId = FGuid::NewGuid(); C.Kind = Kind; C.Mutation = Resolver.Token;
		auto Replacement = Kind == EKind::Metadata ? F.Metadata() : F.UserData(); Replacement.Order.RemoveAt(0);
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		Action->Preimage = FParentPreimage::Capture(C, *Verified.Lease, Resolver, Replacement, *Transaction, Diagnostics);
		if (!TestTrue(TEXT("strong complete parent preimage captured while Core is Live"), Action->Preimage.IsValid())) return false;
		TestEqual(TEXT("readonly capture did not enlist a lifecycle footprint"), Transaction->GetState(), EAssetDocumentAtomicState::Live);
		TestTrue(TEXT("fresh native baseline"), Action->Preimage->ValidateBaseline(C, *Verified.Lease, Resolver, *Transaction).bValid);
		F.Unchanged(*this);
		Action->Context = C; Action->Transaction = Transaction.Get(); Action->Resolver = &Resolver; Action->Handle = Handle.GetValue(); Action->Validation = V;
		TestTrue(TEXT("one complete parent/package inverse enlisted before public removal"), Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
		if (Kind == EKind::Metadata) F.Parent->RemoveMetaData(F.ManagedMetadata.Get());
		else F.Parent->RemoveUserDataOfClass(F.ManagedData->GetClass());
		Action->ExactOwnedCurrent = {Kind == EKind::Metadata ? static_cast<UObject*>(F.UnmanagedMetadata.Get()) : static_cast<UObject*>(F.UnmanagedData.Get())};
		TestFalse(TEXT("forward managed deletion changes original membership"), Action->Preimage->OriginalAttachmentsCurrent());
		TestTrue(TEXT("other attachment family is retained"), Action->Preimage->OtherAttachmentsCurrent());
		TWeakObjectPtr<UAnimSequence> Parent(F.Parent.Get());
		TWeakObjectPtr<UAssetDocumentSequenceLifecycleMetadataTest> ManagedMetadata(F.ManagedMetadata.Get()), UnmanagedMetadata(F.UnmanagedMetadata.Get());
		TWeakObjectPtr<UAssetDocumentSequenceLifecycleUserDataTest> ManagedData(F.ManagedData.Get());
		TWeakObjectPtr<UAssetDocumentSequenceLifecycleOtherUserDataTest> UnmanagedData(F.UnmanagedData.Get());
		F.ManagedMetadata.Reset(); F.UnmanagedMetadata.Reset(); F.ManagedData.Reset(); F.UnmanagedData.Reset(); F.Parent.Reset(); F.Package.Reset();
		CollectGarbage(RF_NoFlags);
		if (!TestTrue(TEXT("preimage alone strongly retains exact parent through GC"), Parent.IsValid())
			|| !TestTrue(TEXT("detached managed metadata remains inverse-owned"), ManagedMetadata.IsValid())
			|| !TestTrue(TEXT("complete unmanaged metadata identity remains retained"), UnmanagedMetadata.IsValid())
			|| !TestTrue(TEXT("detached managed user data remains inverse-owned"), ManagedData.IsValid())
			|| !TestTrue(TEXT("complete unmanaged user-data identity remains retained"), UnmanagedData.IsValid())) return false;
		F.Parent.Reset(Parent.Get()); F.Package.Reset(Parent->GetOutermost());
		F.ManagedMetadata.Reset(ManagedMetadata.Get()); F.UnmanagedMetadata.Reset(UnmanagedMetadata.Get());
		F.ManagedData.Reset(ManagedData.Get()); F.UnmanagedData.Reset(UnmanagedData.Get());
		TestTrue(TEXT("late failure starts after ReadyToCommit"), Transaction->MarkReadyToCommit().bValid);
		TestTrue(TEXT("real Core rollback restores exact full attachment order"), Transaction->Rollback(TEXT("test.sequence-owned-fragment.late-failure")).bValid);
		TestTrue(TEXT("forward handles were actually revoked before native inverse"), Action->bRevokedBeforeInverse);
		TestEqual(TEXT("Core invalidates registry once"), Resolver.RollbackInvalidations, 1);
		TestFalse(TEXT("revoked offered parent lease cannot recapture native state"),
			FParentPreimage::Capture(C, *Verified.Lease, Resolver, Replacement, *Transaction, Diagnostics).IsValid());
		TestTrue(TEXT("native full parent preimage is restored"), Action->Preimage->OriginalAttachmentsCurrent());
		TestFalse(TEXT("terminal native inverse cannot repeat"), Action->Preimage->RestoreAttachments(C, *Transaction, Action->ExactOwnedCurrent).bValid);
		F.Unchanged(*this);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceOwnedFragmentExistingFieldCoreRollback,
	"UEAssetDoc.AnimationProvider.SequenceOwnedFragmentLifecycle.ExistingFieldCoreRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceOwnedFragmentExistingFieldCoreRollback::RunTest(const FString&)
{
	using namespace AssetDocumentSequenceOwnedFragmentOrderTests;
	for (const EKind Kind : {EKind::Metadata, EKind::AssetUserData})
	for (const bool bDirty : {false, true})
	{
		FFixture F; FResolver Resolver;
		TStrongObjectPtr<UObject> Deleted;
		if (Kind == EKind::Metadata)
		{
			auto* O = NewObject<UAssetDocumentSequenceLifecycleMetadataTest>(F.Parent.Get(), TEXT("DeletedMetadata"));
			Deleted.Reset(O); F.Parent->AddMetaData(O);
		}
		else
		{
			auto* O = NewObject<UAssetDocumentSequenceLifecycleDerivedUserDataTest>(F.Parent.Get(), TEXT("DeletedUserData"));
			Deleted.Reset(O); F.Parent->AddAssetUserData(O);
		}
		F.Package->SetDirtyFlag(bDirty);
		const auto OriginalMetadata = F.Parent->GetMetaData();
		const auto OriginalData = *F.Parent->GetAssetUserDataArray();
		FAssetDocumentRollbackActionComposition Composition(Resolver, {}, [](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic&) {});
		const FString Component = Kind == EKind::Metadata ? TEXT("animation.sequence.metadata.existing-native.inverse.v1")
			: TEXT("animation.sequence.asset-user-data.existing-native.inverse.v1");
		FAssetDocumentPreflightReceipt Receipt; Receipt.RequestId = FGuid::NewGuid(); Receipt.RuntimeRevision = 23;
		Receipt.CompleteFootprint = {{EAssetDocumentMutationFootprintKind::UObjectState, F.Parent->GetPathName(), Component},
			{EAssetDocumentMutationFootprintKind::Package, F.Package->GetName(), Component}};
		Receipt.ReceiptHash = TEXT("test.sequence-owned-fragment.existing-fields"); Receipt.bSucceeded = true;
		FCancellation Cancellation; auto Transaction = Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
		if (!TestTrue(TEXT("real existing-field Core transaction"), Transaction.IsValid())) return false;
		Resolver.Token = Transaction->GetMutationToken(); auto Handle = Resolver.ResolveParent(*F.Parent.Get(), *Transaction);
		if (!TestTrue(TEXT("Editor immutable parent issuance"), Handle.IsSet())) return false;
		FAssetDocumentHandleValidationContext V; V.RequestId = Resolver.Token.RequestId; V.TransactionId = Resolver.Token.TransactionId;
		V.RuntimeRevision = Resolver.Token.RuntimeRevision; V.ProviderId = TEXT("animation"); V.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
		V.RequiredKind = EAssetDocumentHandleKind::Asset; V.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
		auto Verified = Resolver.AcquireVerifiedLease(Handle.GetValue(), V);
		if (!TestTrue(TEXT("verified exact parent"), Verified.Lease.IsValid())) return false;
		FAssetDocumentEditorSequenceFragmentContext C; C.OperationId = FGuid::NewGuid(); C.Kind = Kind; C.Mutation = Resolver.Token;
		auto Replacement = Kind == EKind::Metadata ? F.Metadata() : F.UserData(); Replacement.Managed.Add(Deleted.Get());
		Replacement.Order[0].Name = Replacement.Order[0].Existing->GetName();
		auto& P = Replacement.Order[0].Properties; P.ObjectKeys = {TEXT("First"), TEXT("Label")};
		FAssetDocumentRuntimeValueDto First, Label; First.Kind = EAssetDocumentRuntimeValueKind::Integer; First.IntegerValue = 101;
		Label.Kind = EAssetDocumentRuntimeValueKind::String; Label.StringValue = TEXT("after"); P.Elements = {First, Label};
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		// A malformed later field must fail while Core remains Live. The complete
		// native parent and the first field remain unchanged, with no enlistment.
		auto Bad = Replacement; Bad.Order[0].Properties.Elements[1] = First;
		TestFalse(TEXT("late invalid field decode rejects the whole existing operation"),
			FExistingReplacement::Prepare(C, *Verified.Lease, Resolver, Bad, *Transaction, Diagnostics).IsValid());
		TestEqual(TEXT("failed preparation retains Live/no-footprint state"), Transaction->GetState(), EAssetDocumentAtomicState::Live);
		TestTrue(TEXT("failed preparation retains all metadata"), F.Parent->GetMetaData() == OriginalMetadata);
		TestTrue(TEXT("failed preparation retains all user data"), *F.Parent->GetAssetUserDataArray() == OriginalData);
		TestEqual(TEXT("early metadata field never written"), F.ManagedMetadata->First, 17);
		TestEqual(TEXT("early user-data field never written"), F.ManagedData->First, 7);
		auto Prepared = FExistingReplacement::Prepare(C, *Verified.Lease, Resolver, Replacement, *Transaction, Diagnostics);
		if (!TestTrue(TEXT("all fields capture/decode before parent footprint"), Prepared.IsValid())) return false;
		TestEqual(TEXT("successful prepare still leaves Core Live"), Transaction->GetState(), EAssetDocumentAtomicState::Live);
		// FString equality ignores case, while fragment Name and native frozen
		// paths are exact. A public case-only Rename preserves pointer/UID/outer,
		// order and dirty state; it must still invalidate the prepared identity.
		for (UObject* Object : {Replacement.Order[0].Existing, static_cast<UObject*>(F.Parent.Get())})
		{
			const FString Name = Object->GetName(), LowerName = Name.ToLower(); const uint32 Id = Object->GetUniqueID();
			constexpr ERenameFlags Flags = REN_NonTransactional | REN_DoNotDirty | REN_DontCreateRedirectors;
			if (!TestTrue(TEXT("test-owned case-only public rename succeeds"), Object->Rename(*LowerName, nullptr, Flags))) return false;
			if (!TestTrue(TEXT("display name actually changed case"), Object->GetName().Equals(LowerName, ESearchCase::CaseSensitive))) return false;
			TestEqual(TEXT("case-only rename preserves native UID"), Object->GetUniqueID(), Id);
			TestFalse(TEXT("prepared apply rejects exact case drift before first enlist"), Prepared->Apply(C, *Verified.Lease, Resolver, *Transaction).bValid);
			TestEqual(TEXT("case drift rejection leaves Core Live"), Transaction->GetState(), EAssetDocumentAtomicState::Live);
			TestTrue(TEXT("case drift rejection retains metadata membership/order"), F.Parent->GetMetaData() == OriginalMetadata);
			TestTrue(TEXT("case drift rejection retains user-data membership/order"), *F.Parent->GetAssetUserDataArray() == OriginalData);
			TestEqual(TEXT("case drift rejection leaves early metadata field untouched"), F.ManagedMetadata->First, 17);
			TestEqual(TEXT("case drift rejection leaves early user-data field untouched"), F.ManagedData->First, 7);
			TestEqual(TEXT("case drift rejection preserves dirty state"), F.Package->IsDirty(), bDirty);
			if (!TestTrue(TEXT("test restores exact original display name"), Object->Rename(*Name, nullptr, Flags)
				&& Object->GetName().Equals(Name, ESearchCase::CaseSensitive))) return false;
		}
		TestTrue(TEXT("exact existing-field apply enlists and composes public publication"), Prepared->Apply(C, *Verified.Lease, Resolver, *Transaction).bValid);
		if (Kind == EKind::Metadata)
		{
			TestEqual(TEXT("managed metadata updated"), F.ManagedMetadata->First, 101);
			TestEqual(TEXT("managed metadata text updated"), F.ManagedMetadata->Label, FString(TEXT("after")));
			TestTrue(TEXT("only omitted managed metadata deleted"), F.Parent->GetMetaData() == TArray<UAnimMetaData*>{F.ManagedMetadata.Get(), F.UnmanagedMetadata.Get()});
		}
		else
		{
			TestEqual(TEXT("managed user data updated"), F.ManagedData->First, 101);
			TestEqual(TEXT("managed user-data text updated"), F.ManagedData->Label, FString(TEXT("after")));
			TestTrue(TEXT("only omitted managed user data deleted"), *F.Parent->GetAssetUserDataArray() == TArray<UAssetUserData*>{F.ManagedData.Get(), F.UnmanagedData.Get()});
		}
		TestEqual(TEXT("unmanaged metadata scalar retained"), F.UnmanagedMetadata->First, 29);
		TestTrue(TEXT("unmanaged container retained"), F.UnmanagedMetadata->Values == TArray<int32>{2, 5, 9});
		TestTrue(TEXT("real late failure enters ReadyToCommit"), Transaction->MarkReadyToCommit().bValid);
		TestTrue(TEXT("real Core composes parent inverse before raw field inverse"), Transaction->Rollback(TEXT("test.sequence-owned-fragment.fields-late-failure")).bValid);
		TestFalse(TEXT("real forward parent handle stays revoked"), Resolver.AcquireVerifiedLease(Handle.GetValue(), V).Lease.IsValid());
		TestTrue(TEXT("complete metadata membership/order restored"), F.Parent->GetMetaData() == OriginalMetadata);
		TestTrue(TEXT("complete user-data membership/order restored"), *F.Parent->GetAssetUserDataArray() == OriginalData);
		TestEqual(TEXT("metadata native scalar restored"), F.ManagedMetadata->First, 17);
		TestEqual(TEXT("user-data native scalar restored"), F.ManagedData->First, 7);
		TestEqual(TEXT("metadata native text restored"), F.ManagedMetadata->Label, FString(TEXT("before")));
		TestEqual(TEXT("user-data native text restored"), F.ManagedData->Label, FString(TEXT("before")));
		TestEqual(TEXT("raw operation retains the original package dirty flag"), F.Package->IsDirty(), bDirty);
		TestFalse(TEXT("existing inverse is consumed"), Prepared->Rollback(TEXT("test.repeat")).bValid);
	}
	return true;
}

// A boundary control, not successful lifecycle rollback or class admission.
// Stock allocation reaches a creation-only observer whose non-UObject state is
// untouched by parent restoration and object revocation. Production must close
// its exact creation-effect ownership before enabling the create branch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceOwnedFragmentCreationEffectsControl,
	"UEAssetDoc.AnimationProvider.SequenceOwnedFragmentLifecycle.CreationEffectsControl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceOwnedFragmentCreationEffectsControl::RunTest(const FString&)
{
	using namespace AssetDocumentSequenceOwnedFragmentOrderTests;
	FFixture F;
	FCreationObserver Observer(*F.Parent.Get(), *UAssetDocumentSequenceLifecycleDerivedUserDataTest::StaticClass());
	TestTrue(TEXT("actual public stock class helper creates and attaches"),
		F.Parent->AddAssetUserDataOfClass(UAssetDocumentSequenceLifecycleDerivedUserDataTest::StaticClass()));
	auto* Created = F.Parent->GetAssetUserDataOfClass(UAssetDocumentSequenceLifecycleDerivedUserDataTest::StaticClass());
	if (!TestNotNull(TEXT("exact new helper-created child"), Created)) return false;
	TestEqual(TEXT("helper allocation actually reaches creation-only observer"), Observer.Count, 1);
	TWeakObjectPtr<UObject> Weak(Created);
	F.Parent->RemoveUserDataOfClass(Created->GetClass());
	F.Unchanged(*this);
	// This exact newly created fixture object is detached. Revocation is only a
	// counterexample control; it is not the production no-surviving-reference proof.
	Created->MarkAsGarbage();
	TestFalse(TEXT("fixture weak identity revoked"), Weak.IsValid());
	TestEqual(TEXT("parent restoration and revocation do not inverse the external observer"), Observer.Count, 1);
	return true;
}
#endif
