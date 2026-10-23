// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Sequence/AssetDocumentAnimationSequenceOwnedFragmentLifecycle.h"

#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimSequence.h"
#include "Engine/AssetUserData.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectThreadContext.h"

namespace AssetDocumentSequenceOwnedFragmentLifecycle
{
namespace
{
using EKind = EAssetDocumentEditorSequenceFragmentKind;
bool ExactText(FStringView A, FStringView B) { return A.Equals(B, ESearchCase::CaseSensitive); }
FAssetDocumentContractValidationResult Result(bool Valid, const TCHAR* Code = TEXT("SequenceOwnedFragment.Lifecycle.InvalidState"))
{
	FAssetDocumentContractValidationResult R; R.bValid = Valid;
	if (!Valid)
	{
		auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/SequenceOwnedFragment"); D.Code = Code;
		D.Message = TEXT("The exact owned-fragment parent preimage or public attachment operation was rejected.");
	}
	return R;
}
bool Safe()
{
	return GIsEditor && IsInGameThread() && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr)
		&& !FUObjectThreadContext::Get().IsRoutingPostLoad;
}
bool Live(const UObject* O)
{
	return IsValid(O) && !O->IsTemplate()
		&& !O->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_BeginDestroyed | RF_FinishDestroyed)
		&& !O->HasAnyInternalFlags(EInternalObjectFlags::AsyncLoadingPhase1);
}
bool UsableClass(const UClass* C, EKind Kind)
{
	const UClass* Base = Kind == EKind::Metadata ? UAnimMetaData::StaticClass() : UAssetUserData::StaticClass();
	return IsValid(C) && !C->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed)
		&& !C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		&& C->IsChildOf(Base);
}
TArray<UObject*> ReadOrder(const UAnimSequence& Parent, EKind Kind)
{
	TArray<UObject*> Out;
	if (Kind == EKind::Metadata) for (auto* O : Parent.GetMetaData()) Out.Add(O);
	else if (const auto* Data = Parent.GetAssetUserDataArray()) for (auto* O : *Data) Out.Add(O);
	return Out;
}
EKind Other(EKind Kind) { return Kind == EKind::Metadata ? EKind::AssetUserData : EKind::Metadata; }
bool SameContext(const FAssetDocumentEditorSequenceFragmentContext& A, const FAssetDocumentEditorSequenceFragmentContext& B)
{
	return A.OperationId == B.OperationId && A.Kind == B.Kind && A.Phase == B.Phase
		&& A.Mutation.RequestId == B.Mutation.RequestId && A.Mutation.TransactionId == B.Mutation.TransactionId
		&& A.Mutation.RuntimeRevision == B.Mutation.RuntimeRevision;
}
bool TransactionMatches(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentAtomicTransaction& T, bool Inverse)
{
	const auto& M = T.GetMutationToken();
	return Safe() && C.OperationId.IsValid() && C.Phase == EAssetDocumentRuntimeRequestPhase::Apply
		&& C.Mutation.RequestId.IsValid() && C.Mutation.TransactionId.IsValid() && C.Mutation.RuntimeRevision != 0
		&& T.GetTransactionId() == C.Mutation.TransactionId && M.RequestId == C.Mutation.RequestId
		&& M.TransactionId == C.Mutation.TransactionId && M.RuntimeRevision == C.Mutation.RuntimeRevision
		&& ((!Inverse && T.GetState() == EAssetDocumentAtomicState::Live) || T.GetState() == EAssetDocumentAtomicState::Mutating
			|| (Inverse && T.GetState() == EAssetDocumentAtomicState::ReadyToCommit));
}
bool SameHandle(const FAssetDocumentOpaqueHandle& A, const FAssetDocumentOpaqueHandle& B)
{
	return A.RequestId == B.RequestId && A.TransactionId == B.TransactionId && A.RuntimeRevision == B.RuntimeRevision
		&& ExactText(A.ProviderId, B.ProviderId) && A.Kind == B.Kind && A.Access == B.Access && A.Slot == B.Slot && A.Generation == B.Generation;
}
FAssetDocumentVerifiedLeaseResult Acquire(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Offered, IAssetDocumentHandleResolver& Handles)
{
	const auto& H = Offered.GetHandle();
	if (!Safe() || !ExactText(H.ProviderId, TEXT("animation")) || H.Kind != EAssetDocumentHandleKind::Asset
		|| H.Access != EAssetDocumentHandleAccess::Mutable || !H.Slot || !H.Generation
		|| H.RequestId != C.Mutation.RequestId || H.TransactionId != C.Mutation.TransactionId
		|| H.RuntimeRevision != C.Mutation.RuntimeRevision) return {};
	FAssetDocumentHandleValidationContext V;
	V.RequestId = H.RequestId; V.TransactionId = H.TransactionId; V.RuntimeRevision = H.RuntimeRevision;
	V.ProviderId = TEXT("animation"); V.Phase = C.Phase; V.RequiredKind = EAssetDocumentHandleKind::Asset;
	V.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Handles.AcquireVerifiedLease(H, V);
}
bool ClassSelectorsLossless(TConstArrayView<UClass*> Classes)
{
	// Simulate the exact public Add rule: a later base/equal class would evict an
	// earlier derived/equal class. Base-before-derived order is lossless and its
	// Get/Remove selectors still select the exact first object of each class.
	for (int32 I = 0; I < Classes.Num(); ++I)
		for (int32 J = 0; J < I; ++J)
			if (Classes[J]->IsChildOf(Classes[I])) return false;
	return true;
}
struct FObjectIdentity
{
	TStrongObjectPtr<UObject> Object;
	TStrongObjectPtr<UClass> Class;
	FString Path, ClassPath;
	uint32 Revision, ClassRevision;
	explicit FObjectIdentity(UObject& O) : Object(&O), Class(O.GetClass()), Path(O.GetPathName()),
		ClassPath(O.GetClass()->GetPathName()), Revision(O.GetUniqueID()), ClassRevision(O.GetClass()->GetUniqueID()) {}
	bool Current(UObject& Parent) const
	{
		return Live(Object.Get()) && Object->GetOuter() == &Parent && Object->GetClass() == Class.Get()
			&& Object->GetUniqueID() == Revision && ExactText(Object->GetPathName(), Path)
			&& IsValid(Class.Get()) && Class->GetUniqueID() == ClassRevision && ExactText(Class->GetPathName(), ClassPath);
	}
};
}

FAssetDocumentContractValidationResult ValidateAttachmentOrder(const UAnimSequence& Parent, const FReplacement& Replacement)
{
	if (!Safe() || !Live(&Parent) || Parent.GetClass() != UAnimSequence::StaticClass()
		|| (Replacement.Kind != EKind::Metadata && Replacement.Kind != EKind::AssetUserData)) return Result(false);
	const auto Before = ReadOrder(Parent, Replacement.Kind);
	TSet<UObject*> Members, Managed, DesiredObjects;
	TArray<UClass*> BeforeClasses, DesiredClasses;
	TArray<UObject*> UnmanagedBefore, UnmanagedAfter;
	for (auto* O : Before)
	{
		if (!Live(O) || O->GetOuter() != &Parent || !UsableClass(O->GetClass(), Replacement.Kind) || Members.Contains(O))
			return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PreimageUnrepresentable"));
		Members.Add(O); BeforeClasses.Add(O->GetClass());
	}
	for (auto* O : Replacement.Managed)
	{
		if (!Members.Contains(O) || Managed.Contains(O)) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.ManagedIdentity"));
		Managed.Add(O);
	}
	for (auto* O : Before) if (!Managed.Contains(O)) UnmanagedBefore.Add(O);
	for (const auto& Entry : Replacement.Order)
	{
		if ((Entry.Existing != nullptr) == (Entry.CreateClass != nullptr)
			|| Entry.Properties.Kind != EAssetDocumentRuntimeValueKind::Object
			|| !ValidateAssetDocumentRuntimeValue(Entry.Properties).bValid)
			return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.InputShape"));
		if (Entry.CreateClass)
		{
			if (Entry.Name.IsSet()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.ExplicitCreateNameUnsupported"));
			if (!UsableClass(Entry.CreateClass, Replacement.Kind)) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.CreateClassUnsupported"));
			DesiredClasses.Add(Entry.CreateClass);
		}
		else
		{
			if (!Members.Contains(Entry.Existing) || DesiredObjects.Contains(Entry.Existing)
				|| (Entry.Name.IsSet() && !Entry.Name.GetValue().Equals(Entry.Existing->GetName(), ESearchCase::CaseSensitive)))
				return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.ExistingIdentity"));
			DesiredObjects.Add(Entry.Existing); DesiredClasses.Add(Entry.Existing->GetClass());
			if (!Managed.Contains(Entry.Existing))
			{
				if (!Entry.Properties.ObjectKeys.IsEmpty()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.UnmanagedFieldWrite"));
				UnmanagedAfter.Add(Entry.Existing);
			}
		}
	}
	if (UnmanagedBefore != UnmanagedAfter) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.UnmanagedOrder"));
	if (Replacement.Kind == EKind::AssetUserData
		&& (!ClassSelectorsLossless(BeforeClasses) || !ClassSelectorsLossless(DesiredClasses)))
		return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.ClassSelectorCollision"));
	return Result(true);
}

struct FParentPreimage::FImpl
{
	FAssetDocumentEditorSequenceFragmentContext Context;
	FAssetDocumentOpaqueHandle Handle;
	const IAssetDocumentAtomicTransaction* Transaction = nullptr;
	FReplacement Replacement;
	TStrongObjectPtr<UAnimSequence> Parent;
	TStrongObjectPtr<UPackage> Package;
	FString ParentPath, PackagePath;
	uint32 ParentRevision = 0, PackageRevision = 0;
	TArray<FObjectIdentity> Original, OtherObjects;
	TArray<TStrongObjectPtr<UClass>> CreateClasses;
	TArray<FString> ClassPaths;
	TArray<uint32> ClassRevisions;
	bool bPackageDirty = false, bInverseAttempted = false;
	bool IdentityCurrent() const
	{
		return Live(Parent.Get()) && Parent->GetClass() == UAnimSequence::StaticClass()
			&& Parent->GetUniqueID() == ParentRevision && ExactText(Parent->GetPathName(), ParentPath)
			&& Live(Package.Get()) && Parent->GetOutermost() == Package.Get()
			&& Package->GetUniqueID() == PackageRevision && ExactText(Package->GetPathName(), PackagePath);
	}
	bool ObjectsCurrent(const TArray<FObjectIdentity>& Objects) const
	{
		if (!IdentityCurrent()) return false;
		for (const auto& O : Objects) if (!O.Current(*Parent.Get())) return false;
		return true;
	}
	TArray<UObject*> Pointers(const TArray<FObjectIdentity>& Objects) const
	{
		TArray<UObject*> Out; for (const auto& O : Objects) Out.Add(O.Object.Get()); return Out;
	}
	bool ClassesCurrent() const
	{
		for (int32 I = 0; I < CreateClasses.Num(); ++I)
			if (!UsableClass(CreateClasses[I].Get(), Context.Kind) || CreateClasses[I]->GetUniqueID() != ClassRevisions[I]
				|| !ExactText(CreateClasses[I]->GetPathName(), ClassPaths[I])) return false;
		return true;
	}
};

FParentPreimage::FParentPreimage(TUniquePtr<FImpl> InImpl) : Impl(MoveTemp(InImpl)) {}
FParentPreimage::~FParentPreimage() = default;

TUniquePtr<FParentPreimage> FParentPreimage::Capture(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles,
	const FReplacement& Replacement, IAssetDocumentAtomicTransaction& T, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	auto Reject = [&](const FAssetDocumentContractValidationResult& R) -> TUniquePtr<FParentPreimage>
	{ Diagnostics.Append(R.Diagnostics); return {}; };
	if (C.Kind != Replacement.Kind || !TransactionMatches(C, T, false)) return Reject(Result(false));
	auto Verified = Acquire(C, Lease, Handles);
	if (!Verified.Lease) return Reject(Result(false, TEXT("SequenceOwnedFragment.Lifecycle.ParentLease")));
	auto S = MakeUnique<FImpl>(); S->Context = C; S->Handle = Lease.GetHandle(); S->Transaction = &T; S->Replacement = Replacement;
	auto R = WithAssetDocumentEditorAssetIdentity(*Verified.Lease, [&](const FAssetDocumentEditorAssetIdentityView& V)
	{
		auto* Parent = Cast<UAnimSequence>(&V.GetObject());
		if (!Parent || Parent->GetClass() != UAnimSequence::StaticClass() || V.GetTypeRevision() != Parent->GetUniqueID()
			|| !ExactText(V.GetCanonicalObjectPath(), Parent->GetPathName())) return Result(false);
		auto Valid = ValidateAttachmentOrder(*Parent, Replacement); if (!Valid.bValid) return Valid;
		S->Parent.Reset(Parent); S->Package.Reset(Parent->GetOutermost());
		S->ParentPath = Parent->GetPathName(); S->ParentRevision = Parent->GetUniqueID();
		S->PackagePath = S->Package->GetPathName(); S->PackageRevision = S->Package->GetUniqueID(); S->bPackageDirty = S->Package->IsDirty();
		for (auto* O : ReadOrder(*Parent, C.Kind)) S->Original.Emplace(*O);
		for (auto* O : ReadOrder(*Parent, Other(C.Kind)))
		{
			if (!Live(O) || O->GetOuter() != Parent) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PreimageUnrepresentable"));
			S->OtherObjects.Emplace(*O);
		}
		for (const auto& Entry : Replacement.Order) if (Entry.CreateClass)
		{
			S->CreateClasses.Emplace(Entry.CreateClass); S->ClassPaths.Add(Entry.CreateClass->GetPathName()); S->ClassRevisions.Add(Entry.CreateClass->GetUniqueID());
		}
		return Result(S->IdentityCurrent() && S->ObjectsCurrent(S->Original) && S->ObjectsCurrent(S->OtherObjects) && S->ClassesCurrent());
	});
	if (!R.bValid) return Reject(R);
	return TUniquePtr<FParentPreimage>(new FParentPreimage(MoveTemp(S)));
}

bool FParentPreimage::OriginalAttachmentsCurrent() const
{
	return Impl->ObjectsCurrent(Impl->Original) && ReadOrder(*Impl->Parent.Get(), Impl->Context.Kind) == Impl->Pointers(Impl->Original);
}
bool FParentPreimage::OtherAttachmentsCurrent() const
{
	return Impl->ObjectsCurrent(Impl->OtherObjects) && ReadOrder(*Impl->Parent.Get(), Other(Impl->Context.Kind)) == Impl->Pointers(Impl->OtherObjects);
}
UAnimSequence& FParentPreimage::GetParent() const { return *Impl->Parent.Get(); }
const FReplacement& FParentPreimage::GetReplacement() const { return Impl->Replacement; }
TArray<UObject*> FParentPreimage::GetOriginalOrder() const { return Impl->Pointers(Impl->Original); }
bool FParentPreimage::WasPackageDirty() const { return Impl->bPackageDirty; }

FAssetDocumentContractValidationResult FParentPreimage::ValidateBaseline(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction& T) const
{
	auto R = ValidateIdentity(C, Lease, Handles, T); if (!R.bValid) return R;
	if (!OriginalAttachmentsCurrent()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PreimageDrift"));
	return Result(true);
}

FAssetDocumentContractValidationResult FParentPreimage::ValidateIdentity(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction& T) const
{
	if (Impl->bInverseAttempted || !SameContext(C, Impl->Context) || !SameHandle(Lease.GetHandle(), Impl->Handle)
		|| &T != Impl->Transaction || !TransactionMatches(C, T, false) || !Impl->ObjectsCurrent(Impl->Original)
		|| !OtherAttachmentsCurrent() || !Impl->ClassesCurrent()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PreimageDrift"));
	auto Verified = Acquire(C, Lease, Handles);
	if (!Verified.Lease) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.ParentLease"));
	return WithAssetDocumentEditorAssetIdentity(*Verified.Lease, [&](const FAssetDocumentEditorAssetIdentityView& V)
	{
		return Result(&V.GetObject() == Impl->Parent.Get() && V.GetTypeRevision() == Impl->ParentRevision
			&& ExactText(V.GetCanonicalObjectPath(), Impl->ParentPath) && Impl->IdentityCurrent());
	});
}

FAssetDocumentContractValidationResult FParentPreimage::RestoreAttachments(const FAssetDocumentEditorSequenceFragmentContext& C,
	IAssetDocumentAtomicTransaction& T, TConstArrayView<UObject*> ExactOwnedCurrentOrder)
{
	if (Impl->bInverseAttempted || !SameContext(C, Impl->Context) || &T != Impl->Transaction
		|| !TransactionMatches(C, T, true) || !Impl->ObjectsCurrent(Impl->Original) || !OtherAttachmentsCurrent())
		return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.AttachmentInverseFailed"));
	auto& Parent = *Impl->Parent.Get();
	const auto Current = ReadOrder(Parent, C.Kind);
	if (Current.Num() != ExactOwnedCurrentOrder.Num()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.AttachmentInverseFailed"));
	for (int32 I = 0; I < Current.Num(); ++I)
		if (Current[I] != ExactOwnedCurrentOrder[I] || !Live(Current[I]) || Current[I]->GetOuter() != &Parent
			|| !UsableClass(Current[I]->GetClass(), C.Kind))
			return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.AttachmentInverseFailed"));
	// Never erase an unexpected attachment introduced outside the recorded public
	// helper chain. The owner retains this exact current order after each effect.
	Impl->bInverseAttempted = true;
	if (OriginalAttachmentsCurrent()) return Result(true);
	if (C.Kind == EKind::Metadata)
	{
		Parent.EmptyMetaData();
		for (const auto& O : Impl->Original) Parent.AddMetaData(static_cast<UAnimMetaData*>(O.Object.Get()));
	}
	else
	{
		// Every removal selects the actual first element; no class-only search is
		// assumed to identify a later instance. The original ordered class proof
		// also guarantees Add cannot evict an earlier restored object.
		while (const auto* Data = Parent.GetAssetUserDataArray())
		{
			if (Data->IsEmpty()) break;
			auto* First = (*Data)[0];
			if (!Live(First) || First->GetOuter() != &Parent || Parent.GetAssetUserDataOfClass(First->GetClass()) != First)
				return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.AttachmentInverseFailed"));
			const int32 Count = Data->Num(); Parent.RemoveUserDataOfClass(First->GetClass());
			if (Parent.GetAssetUserDataArray()->Num() != Count - 1) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.AttachmentInverseFailed"));
		}
		for (const auto& O : Impl->Original) Parent.AddAssetUserData(static_cast<UAssetUserData*>(O.Object.Get()));
	}
	return Result(OriginalAttachmentsCurrent() && OtherAttachmentsCurrent(), TEXT("SequenceOwnedFragment.Lifecycle.AttachmentInverseFailed"));
}

struct FExistingReplacement::FImpl
{
	FAssetDocumentEditorSequenceFragmentContext Context;
	IAssetDocumentAtomicTransaction* Transaction = nullptr;
	TUniquePtr<FParentPreimage> Parent;
	TArray<TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties>> Fields;
	TArray<UObject*> CurrentOrder, DesiredOrder;
	bool bApplyAttempted = false, bArmed = false, bInverseAttempted = false;
	bool OrderCurrent() const { return ReadOrder(Parent->GetParent(), Context.Kind) == CurrentOrder; }
	FAssetDocumentContractValidationResult PublishExistingOrder()
	{
		auto& Target = Parent->GetParent();
		if (!OrderCurrent()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PreimageDrift"));
		if (CurrentOrder == DesiredOrder) return Result(true);
		if (Context.Kind == EKind::Metadata)
		{
			CurrentOrder.Reset(); Target.EmptyMetaData();
			if (!OrderCurrent()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PublicationFailed"));
		}
		else while (!CurrentOrder.IsEmpty())
		{
			auto* First = static_cast<UAssetUserData*>(CurrentOrder[0]);
			if (Target.GetAssetUserDataOfClass(First->GetClass()) != First)
				return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PublicationFailed"));
			CurrentOrder.RemoveAt(0); Target.RemoveUserDataOfClass(First->GetClass());
			if (!OrderCurrent()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PublicationFailed"));
		}
		for (auto* O : DesiredOrder)
		{
			CurrentOrder.Add(O);
			if (Context.Kind == EKind::Metadata) Target.AddMetaData(static_cast<UAnimMetaData*>(O));
			else Target.AddAssetUserData(static_cast<UAssetUserData*>(O));
			if (!OrderCurrent()) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PublicationFailed"));
		}
		return Result(Parent->OtherAttachmentsCurrent());
	}
};
FExistingReplacement::FExistingReplacement(TUniquePtr<FImpl> InImpl) : Impl(MoveTemp(InImpl)) {}
FExistingReplacement::~FExistingReplacement() = default;

TSharedPtr<FExistingReplacement> FExistingReplacement::Prepare(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles, const FReplacement& Replacement,
	IAssetDocumentAtomicTransaction& T, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	for (const auto& Entry : Replacement.Order) if (Entry.CreateClass)
	{
		Diagnostics.Append(Result(false, TEXT("SequenceOwnedFragment.Lifecycle.CreationNotInExistingOperation")).Diagnostics); return {};
	}
	auto S = MakeUnique<FImpl>(); S->Context = C; S->Transaction = &T;
	S->Parent = FParentPreimage::Capture(C, Lease, Handles, Replacement, T, Diagnostics);
	if (!S->Parent) return {};
	S->CurrentOrder = S->Parent->GetOriginalOrder();
	for (const auto& Entry : Replacement.Order)
	{
		S->DesiredOrder.Add(Entry.Existing);
		if (!Entry.Properties.ObjectKeys.IsEmpty())
		{
			auto Field = PrepareAssetDocumentEditorSequenceOwnedFragmentProperties(C, Lease, Handles,
				*Entry.Existing, Entry.Properties, T, Diagnostics);
			if (!Field) return {};
			S->Fields.Add(MoveTemp(Field));
		}
	}
	const auto Valid = S->Parent->ValidateBaseline(C, Lease, Handles, T);
	if (!Valid.bValid) { Diagnostics.Append(Valid.Diagnostics); return {}; }
	return MakeShareable(new FExistingReplacement(MoveTemp(S)));
}

FStringView FExistingReplacement::GetRollbackComponentId() const
{
	return Impl->Context.Kind == EKind::Metadata ? TEXTVIEW("animation.sequence.metadata.existing-native.inverse.v1")
		: TEXTVIEW("animation.sequence.asset-user-data.existing-native.inverse.v1");
}
TArray<FAssetDocumentMutationFootprint> FExistingReplacement::GetFootprint() const
{
	const auto& Parent = Impl->Parent->GetParent(); const FString Component(GetRollbackComponentId());
	return {{EAssetDocumentMutationFootprintKind::UObjectState, Parent.GetPathName(), Component},
		{EAssetDocumentMutationFootprintKind::Package, Parent.GetOutermost()->GetName(), Component}};
}

FAssetDocumentContractValidationResult FExistingReplacement::Apply(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction& T)
{
	if (Impl->bApplyAttempted || Impl->bInverseAttempted || &T != Impl->Transaction || !SameContext(C, Impl->Context))
		return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.AlreadyConsumed"));
	auto R = Impl->Parent->ValidateBaseline(C, Lease, Handles, T); if (!R.bValid) return R;
	if (Impl->Parent->GetParent().GetOutermost()->IsDirty() != Impl->Parent->WasPackageDirty())
		return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.PackageStateDrift"));
	Impl->bApplyAttempted = true;
	R = T.EnlistRollbackAction(AsShared()); if (!R.bValid) return R;
	R = T.EnlistFootprint(GetFootprint()); if (!R.bValid) return R;
	Impl->bArmed = true;
	for (auto& Field : Impl->Fields)
	{
		R = Field->ApplyRaw(C, Lease, Handles, T); if (!R.bValid) return R;
	}
	R = Impl->Parent->ValidateIdentity(C, Lease, Handles, T); if (!R.bValid) return R;
	R = Impl->PublishExistingOrder(); if (!R.bValid) return R;
	// Stock pointer publication has no notification or derived-data completion.
	// Its public getters must expose this exact requested order immediately; the pure
	// Prepare already verifies every raw native field value without default callbacks.
	R = Impl->Parent->ValidateIdentity(C, Lease, Handles, T); if (!R.bValid) return R;
	return Result(Impl->Parent->GetParent().GetOutermost()->IsDirty() == Impl->Parent->WasPackageDirty(),
		TEXT("SequenceOwnedFragment.Lifecycle.PackageStateDrift"));
}

FAssetDocumentContractValidationResult FExistingReplacement::Rollback(FStringView)
{
	if (!Impl->bArmed || Impl->bInverseAttempted) return Result(false, TEXT("SequenceOwnedFragment.Lifecycle.AlreadyConsumed"));
	Impl->bInverseAttempted = true;
	auto R = Impl->Parent->RestoreAttachments(Impl->Context, *Impl->Transaction, Impl->CurrentOrder);
	if (!R.bValid) return R;
	for (int32 I = Impl->Fields.Num() - 1; I >= 0; --I)
	{
		R = Impl->Fields[I]->RestoreRaw(Impl->Context, *Impl->Transaction); if (!R.bValid) return R;
	}
	return Result(Impl->Parent->OriginalAttachmentsCurrent() && Impl->Parent->OtherAttachmentsCurrent()
		&& Impl->Parent->GetParent().GetOutermost()->IsDirty() == Impl->Parent->WasPackageDirty());
}
}
