// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentEditorSequenceOwnedFragmentProperties.h"

#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimSequence.h"
#include "Engine/AssetUserData.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "UObject/GCObject.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/PropertyVisitor.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
FAssetDocumentContractValidationResult FragmentFailure(const TCHAR* Code)
{
	FAssetDocumentContractValidationResult R;
	auto& D = R.Diagnostics.AddDefaulted_GetRef();
	D.Path = TEXT("/SequenceOwnedFragment/Properties");
	D.Code = Code;
	D.Message = TEXT("The exact native owned-fragment field operation was rejected.");
	return R;
}
FAssetDocumentContractValidationResult FragmentSuccess()
{
	FAssetDocumentContractValidationResult R; R.bValid = true; return R;
}
bool SameFragmentParentHandle(const FAssetDocumentOpaqueHandle& A, const FAssetDocumentOpaqueHandle& B)
{
	return A.RequestId == B.RequestId && A.TransactionId == B.TransactionId
		&& A.RuntimeRevision == B.RuntimeRevision && A.ProviderId == B.ProviderId
		&& A.Slot == B.Slot && A.Generation == B.Generation && A.Kind == B.Kind && A.Access == B.Access;
}
bool SameFragmentContext(const FAssetDocumentEditorSequenceFragmentContext& A,
	const FAssetDocumentEditorSequenceFragmentContext& B)
{
	return A.OperationId == B.OperationId && A.Mutation.RequestId == B.Mutation.RequestId
		&& A.Mutation.TransactionId == B.Mutation.TransactionId && A.Mutation.RuntimeRevision == B.Mutation.RuntimeRevision
		&& A.Phase == B.Phase && A.Kind == B.Kind;
}
bool LiveFragmentObject(const UObject* O)
{
	return IsValid(O) && !O->IsTemplate()
		&& !O->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed);
}
bool FragmentOperationSafe(const FAssetDocumentEditorSequenceFragmentContext& C, bool bMutable)
{
	return GIsEditor && IsInGameThread() && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr)
		&& !FUObjectThreadContext::Get().IsRoutingPostLoad && C.OperationId.IsValid()
		&& C.Mutation.RequestId.IsValid() && C.Mutation.TransactionId.IsValid() && C.Mutation.RuntimeRevision != 0
		&& (C.Phase == EAssetDocumentRuntimeRequestPhase::Apply || (!bMutable && C.Phase == EAssetDocumentRuntimeRequestPhase::Verify));
}
bool FragmentContextValid(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Lease, bool bMutable)
{
	const auto& H = Lease.GetHandle();
	return FragmentOperationSafe(C, bMutable)
		&& H.Kind == EAssetDocumentHandleKind::Asset && H.ProviderId == TEXT("animation") && H.Slot != 0 && H.Generation != 0
		&& (!bMutable || H.Access == EAssetDocumentHandleAccess::Mutable)
		&& (H.Access == EAssetDocumentHandleAccess::ReadOnly || H.Access == EAssetDocumentHandleAccess::Mutable)
		&& H.RequestId == C.Mutation.RequestId && H.TransactionId == C.Mutation.TransactionId
		&& H.RuntimeRevision == C.Mutation.RuntimeRevision;
}
FAssetDocumentVerifiedLeaseResult AcquireFragmentParent(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Offered, IAssetDocumentHandleResolver& Handles, bool bMutable)
{
	if (!FragmentContextValid(C, Offered, bMutable)) return {};
	FAssetDocumentHandleValidationContext V;
	V.RequestId = C.Mutation.RequestId; V.TransactionId = C.Mutation.TransactionId; V.RuntimeRevision = C.Mutation.RuntimeRevision;
	V.ProviderId = TEXT("animation"); V.Phase = C.Phase; V.RequiredKind = EAssetDocumentHandleKind::Asset;
	V.RequiredAccess = bMutable ? EAssetDocumentHandleAccess::Mutable : Offered.GetHandle().Access;
	// Never dereference an offered lease that may already have been revoked.
	return Handles.AcquireVerifiedLease(Offered.GetHandle(), V);
}
bool FragmentTransactionValid(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentAtomicTransaction& T, bool bInverse)
{
	const auto& M = T.GetMutationToken();
	return T.GetTransactionId() == C.Mutation.TransactionId
		&& M.RequestId == C.Mutation.RequestId && M.TransactionId == C.Mutation.TransactionId
		&& M.RuntimeRevision == C.Mutation.RuntimeRevision
		&& (T.GetState() == EAssetDocumentAtomicState::Mutating
			|| (bInverse && T.GetState() == EAssetDocumentAtomicState::ReadyToCommit));
}
bool FragmentPlanTransactionValid(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentAtomicTransaction& T)
{
	const auto& M = T.GetMutationToken();
	return T.GetTransactionId() == C.Mutation.TransactionId && M.RequestId == C.Mutation.RequestId
		&& M.TransactionId == C.Mutation.TransactionId && M.RuntimeRevision == C.Mutation.RuntimeRevision
		&& (T.GetState() == EAssetDocumentAtomicState::Live || T.GetState() == EAssetDocumentAtomicState::Mutating);
}
bool FragmentClassValid(UClass* Class, EAssetDocumentEditorSequenceFragmentKind Kind)
{
	return IsValid(Class) && !Class->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed)
		&& !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		&& ((Kind == EAssetDocumentEditorSequenceFragmentKind::Metadata && Class->IsChildOf(UAnimMetaData::StaticClass()))
			|| (Kind == EAssetDocumentEditorSequenceFragmentKind::AssetUserData && Class->IsChildOf(UAssetUserData::StaticClass())));
}
struct FSequenceFragmentParentIdentity
{
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UAnimSequence> Parent;
	FString PackagePath, ParentPath;
	uint32 PackageRevision = 0, ParentRevision = 0;
	bool Current() const
	{
		return LiveFragmentObject(Package.Get()) && Package->GetPathName() == PackagePath && Package->GetUniqueID() == PackageRevision
			&& LiveFragmentObject(Parent.Get()) && Parent->GetOutermost() == Package.Get() && Parent->GetClass() == UAnimSequence::StaticClass()
			&& Parent->GetPathName() == ParentPath && Parent->GetUniqueID() == ParentRevision;
	}
};
FAssetDocumentContractValidationResult CaptureFragmentParent(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles, FSequenceFragmentParentIdentity& Out)
{
	auto Current = AcquireFragmentParent(C, Lease, Handles, true);
	if (!Current.Lease) return FragmentFailure(TEXT("SequenceOwnedFragment.InvalidContext"));
	return WithAssetDocumentEditorAssetIdentity(*Current.Lease, [&](const FAssetDocumentEditorAssetIdentityView& V)
	{
		UObject& Parent = V.GetObject();
		if (Parent.GetClass() != UAnimSequence::StaticClass() || V.GetTypeRevision() != Parent.GetUniqueID()
			|| V.GetCanonicalObjectPath() != Parent.GetPathName())
			return FragmentFailure(TEXT("SequenceOwnedFragment.InvalidIdentity"));
		Out.Package.Reset(Parent.GetOutermost()); Out.Parent.Reset(static_cast<UAnimSequence*>(&Parent));
		Out.PackagePath = Out.Package->GetPathName(); Out.PackageRevision = Out.Package->GetUniqueID();
		Out.ParentPath = Parent.GetPathName(); Out.ParentRevision = Parent.GetUniqueID();
		return Out.Current() ? FragmentSuccess() : FragmentFailure(TEXT("SequenceOwnedFragment.InvalidIdentity"));
	});
}
bool FragmentAttachedOnce(const UAnimSequence& Parent, const UObject& Child, EAssetDocumentEditorSequenceFragmentKind Kind)
{
	int32 Count = 0;
	if (Kind == EAssetDocumentEditorSequenceFragmentKind::Metadata && Child.IsA(UAnimMetaData::StaticClass()))
	{
		for (const auto* O : Parent.GetMetaData()) if (O == &Child) ++Count;
	}
	else if (Kind == EAssetDocumentEditorSequenceFragmentKind::AssetUserData && Child.IsA(UAssetUserData::StaticClass()))
	{
		if (const auto* Data = Parent.GetAssetUserDataArray()) for (const auto* O : *Data) if (O == &Child) ++Count;
	}
	return Count == 1;
}
struct FSequenceFragmentIdentity
{
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UAnimSequence> Parent;
	TStrongObjectPtr<UObject> Child;
	TStrongObjectPtr<UClass> ChildClass;
	FString PackagePath, ParentPath, ChildPath, ClassPath;
	uint32 PackageRevision = 0, ParentRevision = 0, ChildRevision = 0, ClassRevision = 0;
	EAssetDocumentEditorSequenceFragmentKind Kind = EAssetDocumentEditorSequenceFragmentKind::Metadata;
	bool Current() const
	{
		return LiveFragmentObject(Package.Get()) && Package->GetPathName() == PackagePath && Package->GetUniqueID() == PackageRevision
			&& LiveFragmentObject(Parent.Get()) && Parent->GetOutermost() == Package.Get() && Parent->GetClass() == UAnimSequence::StaticClass()
			&& Parent->GetPathName() == ParentPath && Parent->GetUniqueID() == ParentRevision
			&& LiveFragmentObject(Child.Get()) && Child->GetOuter() == Parent.Get() && Child->GetClass() == ChildClass.Get()
			&& Child->GetPathName() == ChildPath && Child->GetUniqueID() == ChildRevision
			&& IsValid(ChildClass.Get()) && ChildClass->GetPathName() == ClassPath && ChildClass->GetUniqueID() == ClassRevision
			&& !ChildClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
			&& FragmentAttachedOnce(*Parent.Get(), *Child.Get(), Kind);
	}
};
FAssetDocumentContractValidationResult CaptureIdentity(const FAssetDocumentEditorSequenceFragmentContext& C,
	const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles, UObject& Child, FSequenceFragmentIdentity& Out)
{
	auto Current = AcquireFragmentParent(C, Lease, Handles, false);
	if (!Current.Lease) return FragmentFailure(TEXT("SequenceOwnedFragment.InvalidContext"));
	auto R = WithAssetDocumentEditorAssetIdentity(*Current.Lease, [&](const FAssetDocumentEditorAssetIdentityView& V)
	{
		UObject& Parent = V.GetObject();
		if (Parent.GetClass() != UAnimSequence::StaticClass() || V.GetTypeRevision() != Parent.GetUniqueID()
			|| V.GetCanonicalObjectPath() != Parent.GetPathName() || !LiveFragmentObject(&Child) || Child.GetOuter() != &Parent)
			return FragmentFailure(TEXT("SequenceOwnedFragment.InvalidIdentity"));
		Out.Package.Reset(Parent.GetOutermost()); Out.PackagePath = Out.Package->GetPathName(); Out.PackageRevision = Out.Package->GetUniqueID();
		Out.Parent.Reset(static_cast<UAnimSequence*>(&Parent)); Out.Child.Reset(&Child); Out.ChildClass.Reset(Child.GetClass());
		Out.ParentPath = Parent.GetPathName(); Out.ChildPath = Child.GetPathName(); Out.ClassPath = Child.GetClass()->GetPathName();
		Out.ParentRevision = Parent.GetUniqueID(); Out.ChildRevision = Child.GetUniqueID(); Out.ClassRevision = Child.GetClass()->GetUniqueID(); Out.Kind = C.Kind;
		return Out.Current() ? FragmentSuccess() : FragmentFailure(TEXT("SequenceOwnedFragment.InvalidAttachment"));
	});
	return R;
}
struct FSequenceFragmentField
{
	FString Name;
	TStrongObjectPtr<UClass> DeclaringClass;
	TUniquePtr<FAssetDocumentEditorPropertyTokenPayload> Token;
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Before;
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Desired;
};
bool CurrentFragmentField(const FSequenceFragmentIdentity& Identity, const FSequenceFragmentField& F, FAssetDocumentEditorPropertyTokenView& V)
{
	if (!F.Token->TryGetView(V) || !V.bReadable || !V.bWritable || !Identity.Child->IsA(V.DeclaringClass)
		|| !V.Property || V.Property->GetAuthoredName() != F.Name
		|| PropertyAccessUtil::CanGetPropertyValue(V.Property) != EPropertyAccessResultFlags::Success
		|| PropertyAccessUtil::CanSetPropertyValue(V.Property, PropertyAccessUtil::EditorReadOnlyFlags,
			PropertyAccessUtil::IsObjectTemplate(Identity.Child.Get())) != EPropertyAccessResultFlags::Success)
		return false;
	const auto Codec = InspectAssetDocumentEditorTypedValueProperty(*V.Property);
	return Codec.bSucceeded && Codec.CodecId == V.CodecId;
}
FAssetDocumentContractValidationResult AdmitFragmentFields(UClass& ExactClass,
	TConstArrayView<FString> Names, TArray<FSequenceFragmentField>& Out)
{
	const auto Admitted = GetAssetDocumentEditorAdmittedPropertyFields(ExactClass);
	TSet<FString> Seen;
	for (const auto& Name : Names)
	{
		if (Seen.Contains(Name)) return FragmentFailure(TEXT("SequenceOwnedFragment.DuplicateField"));
		Seen.Add(Name);
		const auto* A = Admitted.FindByPredicate([&](const auto& F) { return F.Property && F.Property->GetAuthoredName() == Name; });
		if (!A || !A->bReadable || !A->bWritable) return FragmentFailure(TEXT("SequenceOwnedFragment.UnsupportedField"));
		// The raw forward/inverse setters restore by value identity, which
		// instanced subobjects do not have.
		if (DoesAssetDocumentEditorPropertyHoldInstancedObjects(*A->Property))
			return FragmentFailure(TEXT("SequenceOwnedFragment.UnsupportedField"));
		auto& F = Out.AddDefaulted_GetRef(); F.Name = Name; F.DeclaringClass.Reset(A->DeclaringClass);
		F.Token = MakeUnique<FAssetDocumentEditorPropertyTokenPayload>(*A->DeclaringClass, *A->Property, A->CodecId, A->bReadable, A->bWritable, A->TypeRevision);
	}
	return FragmentSuccess();
}
bool CurrentFragmentClassField(UClass& Class, const FSequenceFragmentField& F, FAssetDocumentEditorPropertyTokenView& V)
{
	if (!F.Token->TryGetView(V) || !V.bReadable || !V.bWritable || !Class.IsChildOf(V.DeclaringClass)
		|| V.DeclaringClass != F.DeclaringClass.Get() || !V.Property || V.Property->GetAuthoredName() != F.Name)
		return false;
	// Reuse the supported class admission projection, including current flags.
	// Actual template/instance access is deliberately checked only at Bind.
	const auto Admitted = GetAssetDocumentEditorAdmittedPropertyFields(Class);
	const auto* A = Admitted.FindByPredicate([&](const auto& Field) { return Field.Property == V.Property; });
	return A && A->bReadable && A->bWritable && A->DeclaringClass == V.DeclaringClass
		&& A->CodecId == V.CodecId && A->TypeRevision == V.TypeRevision;
}
// Scoped values are not UPROPERTY storage. The stock visitor locates real
// struct-value instances; the public recursive ARO collector owns traversal.
void CollectFragmentFields(TArray<FSequenceFragmentField>& Fields, FReferenceCollector& Collector)
{
	for (auto& F : Fields)
		for (auto* V : {F.Before.Get(), F.Desired.Get()})
			if (V)
			{
				FStructuredArchiveFromArchive Archive(Collector.GetVerySlowReferenceCollectorArchive());
				auto Stream = Archive.GetSlot().EnterStream();
				auto& P = V->GetProperty();
				for (int32 I = 0; I < P.ArrayDim; ++I)
				{
					void* Element = static_cast<uint8*>(V->GetMemory()) + I * P.GetElementSize();
					P.SerializeItem(Stream.EnterElement(), Element, nullptr);
					P.Visit(FPropertyVisitorData(Element, nullptr), [&Collector](const FPropertyVisitorContext& PropertyContext)
					{
						if (const auto* Struct = CastField<FStructProperty>(PropertyContext.Path.Top().Property))
						{
							Collector.AddPropertyReferencesWithStructARO(Struct->Struct.Get(), PropertyContext.Data.PropertyData);
							return EPropertyVisitorControlFlow::StepOver;
						}
						return EPropertyVisitorControlFlow::StepInto;
					});
				}
			}
}
class FFragmentReferenceValidation final : public FReferenceCollector
{
public:
	bool bValid = true;
	bool IsIgnoringArchetypeRef() const override { return false; }
	bool IsIgnoringTransient() const override { return false; }
protected:
	void HandleObjectReference(UObject*& Object, const UObject*, const FProperty*) override
	{
		// A read-only collector: never eliminates or replaces references.
		if (Object && (!IsValid(Object) || Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed))) bValid = false;
	}
};
bool GetFragmentValue(UObject& Child, FProperty& P, FAssetDocumentEditorScopedPropertyValue& Out)
{
	return Out.IsValid() && PropertyAccessUtil::GetPropertyValue_Object(&P, &Child, &P, Out.GetMemory(), INDEX_NONE) == EPropertyAccessResultFlags::Success;
}
bool SameFragmentValue(FProperty& P, const FAssetDocumentEditorScopedPropertyValue& A,
	const FAssetDocumentEditorScopedPropertyValue& B)
{
	return PropertyAccessUtil::IsCompletePropertyIdentical(&P, A.GetMemory(), &P, B.GetMemory());
}
// Both forward and inverse use the same supported raw setter, never Default.
bool SetFragmentRaw(UObject& Child, FProperty& P, const FAssetDocumentEditorScopedPropertyValue& Value)
{
	return &Value.GetProperty() == &P && PropertyAccessUtil::SetPropertyValue_Object(&P, &Child, &P, Value.GetMemory(),
		INDEX_NONE, PropertyAccessUtil::EditorReadOnlyFlags, EPropertyAccessChangeNotifyMode::Never) == EPropertyAccessResultFlags::Success;
}
} // namespace

struct FAssetDocumentEditorSequenceOwnedFragmentProperties::FImpl final : FGCObject
{
	FAssetDocumentEditorSequenceFragmentContext Context;
	FAssetDocumentOpaqueHandle ParentHandle;
	const IAssetDocumentAtomicTransaction* Transaction = nullptr;
	FSequenceFragmentIdentity Identity;
	TArray<FSequenceFragmentField> Fields;
	int32 Written = 0;
	bool bApplyAttempted = false, bInverseAttempted = false;
	void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		CollectFragmentFields(Fields, Collector);
	}
	FString GetReferencerName() const override { return TEXT("AssetDocument.SequenceOwnedFragmentFields"); }
	FAssetDocumentContractValidationResult ValidateNative(const FAssetDocumentEditorSequenceFragmentContext& C,
		const IAssetDocumentAtomicTransaction& T, bool bInverse, bool bPreparing = false) const
	{
		if (&T != Transaction || !SameFragmentContext(Context, C) || !FragmentOperationSafe(C, true)
			|| !(bPreparing ? FragmentPlanTransactionValid(C, T) : FragmentTransactionValid(C, T, bInverse)) || !Identity.Current())
			return FragmentFailure(TEXT("SequenceOwnedFragment.StaleParticipant"));
		for (const auto& F : Fields)
		{
			FAssetDocumentEditorPropertyTokenView V;
			if (!CurrentFragmentField(Identity, F, V) || !F.Before || !F.Desired
				|| &F.Before->GetProperty() != V.Property || &F.Desired->GetProperty() != V.Property)
				return FragmentFailure(TEXT("SequenceOwnedFragment.StaleField"));
		}
		return FragmentSuccess();
	}
	FAssetDocumentContractValidationResult ValidateForward(const FAssetDocumentEditorSequenceFragmentContext& C,
		const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles,
		const IAssetDocumentAtomicTransaction& T, bool bPreparing = false) const
	{
		auto R = ValidateNative(C, T, false, bPreparing);
		if (!R.bValid) return R;
		if (!SameFragmentParentHandle(ParentHandle, Lease.GetHandle()) || !FragmentContextValid(C, Lease, true))
			return FragmentFailure(TEXT("SequenceOwnedFragment.StaleParticipant"));
		FSequenceFragmentIdentity CurrentIdentity;
		R = CaptureIdentity(C, Lease, Handles, *Identity.Child.Get(), CurrentIdentity);
		return R.bValid && CurrentIdentity.Parent.Get() == Identity.Parent.Get()
			? FragmentSuccess() : FragmentFailure(TEXT("SequenceOwnedFragment.StaleParticipant"));
	}
};

FAssetDocumentEditorSequenceOwnedFragmentProperties::FAssetDocumentEditorSequenceOwnedFragmentProperties(TUniquePtr<FImpl> InImpl)
	: Impl(MoveTemp(InImpl)) {}
FAssetDocumentEditorSequenceOwnedFragmentProperties::~FAssetDocumentEditorSequenceOwnedFragmentProperties() = default;

struct FAssetDocumentEditorSequenceOwnedFragmentClassPlan::FImpl final : FGCObject
{
	FAssetDocumentEditorSequenceFragmentContext Context;
	FAssetDocumentOpaqueHandle ParentHandle;
	const IAssetDocumentAtomicTransaction* Transaction = nullptr;
	FSequenceFragmentParentIdentity Identity;
	TStrongObjectPtr<UClass> Class;
	FString ClassPath;
	uint32 ClassRevision = 0;
	TArray<FSequenceFragmentField> Fields;
	bool bBindAttempted = false;
	void AddReferencedObjects(FReferenceCollector& Collector) override { CollectFragmentFields(Fields, Collector); }
	FString GetReferencerName() const override { return TEXT("AssetDocument.SequenceOwnedFragmentClassPlan"); }
	FAssetDocumentContractValidationResult Validate(const FAssetDocumentEditorSequenceFragmentContext& C,
		const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles,
		const IAssetDocumentAtomicTransaction& T, bool bRequireDecoded)
	{
		if (&T != Transaction || !SameFragmentContext(Context, C) || !FragmentContextValid(C, Lease, true)
			|| !FragmentPlanTransactionValid(C, T) || !SameFragmentParentHandle(ParentHandle, Lease.GetHandle())
			|| !Identity.Current() || !FragmentClassValid(Class.Get(), C.Kind)
			|| Class->GetPathName() != ClassPath || Class->GetUniqueID() != ClassRevision)
			return FragmentFailure(TEXT("SequenceOwnedFragment.StaleClassPlan"));
		FSequenceFragmentParentIdentity Current;
		auto R = CaptureFragmentParent(C, Lease, Handles, Current);
		if (!R.bValid || Current.Parent.Get() != Identity.Parent.Get() || Current.Package.Get() != Identity.Package.Get())
			return FragmentFailure(TEXT("SequenceOwnedFragment.StaleClassPlan"));
		for (const auto& F : Fields)
		{
			FAssetDocumentEditorPropertyTokenView V;
			if (!CurrentFragmentClassField(*Class.Get(), F, V)
				|| (bRequireDecoded && !F.Desired)
				|| (F.Desired && (!F.Desired->IsValid() || &F.Desired->GetProperty() != V.Property)))
				return FragmentFailure(TEXT("SequenceOwnedFragment.StaleField"));
		}
		FFragmentReferenceValidation References;
		CollectFragmentFields(Fields, References);
		return References.bValid ? FragmentSuccess() : FragmentFailure(TEXT("SequenceOwnedFragment.StaleDesiredReference"));
	}
};

FAssetDocumentEditorSequenceOwnedFragmentClassPlan::FAssetDocumentEditorSequenceOwnedFragmentClassPlan(TUniquePtr<FImpl> InImpl)
	: Impl(MoveTemp(InImpl)) {}
FAssetDocumentEditorSequenceOwnedFragmentClassPlan::~FAssetDocumentEditorSequenceOwnedFragmentClassPlan() = default;

TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentClassPlan> PrepareAssetDocumentEditorSequenceOwnedFragmentClassPlan(
	const FAssetDocumentEditorSequenceFragmentContext& C, const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles,
	UClass& Class, const FAssetDocumentRuntimeValueDto& Properties, IAssetDocumentAtomicTransaction& T,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	auto Reject = [&](const FAssetDocumentContractValidationResult& R) -> TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentClassPlan>
	{ Diagnostics.Append(R.Diagnostics); return nullptr; };
	if (!FragmentContextValid(C, Lease, true) || !FragmentPlanTransactionValid(C, T)) return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.InvalidContext")));
	if (!FragmentClassValid(&Class, C.Kind)) return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.InvalidClass")));
	if (Properties.Kind != EAssetDocumentRuntimeValueKind::Object || !ValidateAssetDocumentRuntimeValue(Properties).bValid)
		return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.InvalidProperties")));
	auto S = MakeUnique<FAssetDocumentEditorSequenceOwnedFragmentClassPlan::FImpl>();
	S->Context = C; S->ParentHandle = Lease.GetHandle(); S->Transaction = &T;
	S->Class.Reset(&Class); S->ClassPath = Class.GetPathName(); S->ClassRevision = Class.GetUniqueID();
	auto R = CaptureFragmentParent(C, Lease, Handles, S->Identity);
	if (!R.bValid) return Reject(R);
	R = AdmitFragmentFields(Class, Properties.ObjectKeys, S->Fields);
	if (!R.bValid) return Reject(R);
	for (int32 I = 0; I < S->Fields.Num(); ++I)
	{
		R = S->Validate(C, Lease, Handles, T, false);
		if (!R.bValid) return Reject(R);
		auto& F = S->Fields[I]; FAssetDocumentEditorPropertyTokenView V;
		if (!CurrentFragmentClassField(Class, F, V)) return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.StaleField")));
		if (!DecodeAssetDocumentEditorTypedValue(*V.Property, Properties.Elements[I], F.Desired).bSucceeded)
			return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.DecodeFailed")));
	}
	R = S->Validate(C, Lease, Handles, T, true);
	if (!R.bValid) return Reject(R);
	return TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentClassPlan>(new FAssetDocumentEditorSequenceOwnedFragmentClassPlan(MoveTemp(S)));
}

FAssetDocumentContractValidationResult FAssetDocumentEditorSequenceOwnedFragmentClassPlan::ValidateBeforeCreate(
	const FAssetDocumentEditorSequenceFragmentContext& C, const IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentHandleResolver& Handles, const IAssetDocumentAtomicTransaction& T) const
{
	return Impl->bBindAttempted ? FragmentFailure(TEXT("SequenceOwnedFragment.AlreadyConsumed")) : Impl->Validate(C, Lease, Handles, T, true);
}

TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties> FAssetDocumentEditorSequenceOwnedFragmentClassPlan::BindOnce(
	const FAssetDocumentEditorSequenceFragmentContext& C, const IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentHandleResolver& Handles, UObject& Child, IAssetDocumentAtomicTransaction& T,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	auto Reject = [&](const FAssetDocumentContractValidationResult& R) -> TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties>
	{ Diagnostics.Append(R.Diagnostics); return nullptr; };
	if (Impl->bBindAttempted) return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.AlreadyConsumed")));
	// No rejected bind can be retried against a different newly supplied child.
	Impl->bBindAttempted = true;
	auto R = Impl->Validate(C, Lease, Handles, T, true);
	if (!R.bValid) return Reject(R);
	if (!FragmentTransactionValid(C, T, false) || Child.GetClass() != Impl->Class.Get())
		return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.InvalidIdentity")));
	auto S = MakeUnique<FAssetDocumentEditorSequenceOwnedFragmentProperties::FImpl>();
	S->Context = C; S->ParentHandle = Lease.GetHandle(); S->Transaction = &T;
	R = CaptureIdentity(C, Lease, Handles, Child, S->Identity);
	if (!R.bValid || S->Identity.Parent.Get() != Impl->Identity.Parent.Get())
		return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.InvalidAttachment")));
	S->Fields = MoveTemp(Impl->Fields);
	for (auto& F : S->Fields)
	{
		FAssetDocumentEditorPropertyTokenView V;
		if (!AcquireFragmentParent(C, Lease, Handles, true).Lease || !FragmentTransactionValid(C, T, false)
			|| !S->Identity.Current() || !CurrentFragmentField(S->Identity, F, V))
			return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.StaleField")));
		F.Before = MakeUnique<FAssetDocumentEditorScopedPropertyValue>(*V.Property);
		if (!GetFragmentValue(Child, *V.Property, *F.Before)) return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.CaptureFailed")));
	}
	R = S->ValidateForward(C, Lease, Handles, T);
	if (!R.bValid) return Reject(R);
	return TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties>(new FAssetDocumentEditorSequenceOwnedFragmentProperties(MoveTemp(S)));
}

TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties> PrepareAssetDocumentEditorSequenceOwnedFragmentProperties(
	const FAssetDocumentEditorSequenceFragmentContext& C, const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles,
	UObject& Child, const FAssetDocumentRuntimeValueDto& Properties, IAssetDocumentAtomicTransaction& T,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	auto Reject = [&](const FAssetDocumentContractValidationResult& R) -> TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties>
	{ Diagnostics.Append(R.Diagnostics); return nullptr; };
	if (!FragmentContextValid(C, Lease, true) || !FragmentPlanTransactionValid(C, T)) return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.InvalidContext")));
	if (Properties.Kind != EAssetDocumentRuntimeValueKind::Object || !ValidateAssetDocumentRuntimeValue(Properties).bValid)
		return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.InvalidProperties")));
	auto S = MakeUnique<FAssetDocumentEditorSequenceOwnedFragmentProperties::FImpl>();
	S->Context = C; S->ParentHandle = Lease.GetHandle(); S->Transaction = &T;
	auto R = CaptureIdentity(C, Lease, Handles, Child, S->Identity);
	if (!R.bValid) return Reject(R);
	R = AdmitFragmentFields(*S->Identity.ChildClass.Get(), Properties.ObjectKeys, S->Fields);
	if (!R.bValid) return Reject(R);
	// Capture every field before decoding can load dependencies. FGCObject keeps
	// native old/new references alive throughout subsequent loads and raw writes.
	for (auto& F : S->Fields)
	{
		FAssetDocumentEditorPropertyTokenView V;
		if (!AcquireFragmentParent(C, Lease, Handles, true).Lease || !FragmentPlanTransactionValid(C, T) || !S->Identity.Current() || !CurrentFragmentField(S->Identity, F, V))
			return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.StaleField")));
		F.Before = MakeUnique<FAssetDocumentEditorScopedPropertyValue>(*V.Property);
		if (!GetFragmentValue(Child, *V.Property, *F.Before)) return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.CaptureFailed")));
	}
	for (int32 I = 0; I < S->Fields.Num(); ++I)
	{
		auto& F = S->Fields[I]; FAssetDocumentEditorPropertyTokenView V;
		if (!AcquireFragmentParent(C, Lease, Handles, true).Lease || !FragmentPlanTransactionValid(C, T) || !S->Identity.Current() || !CurrentFragmentField(S->Identity, F, V))
			return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.StaleField")));
		if (!DecodeAssetDocumentEditorTypedValue(*V.Property, Properties.Elements[I], F.Desired).bSucceeded)
			return Reject(FragmentFailure(TEXT("SequenceOwnedFragment.DecodeFailed")));
	}
	R = S->ValidateForward(C, Lease, Handles, T, true);
	if (!R.bValid) return Reject(R);
	return TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties>(new FAssetDocumentEditorSequenceOwnedFragmentProperties(MoveTemp(S)));
}

FAssetDocumentContractValidationResult FAssetDocumentEditorSequenceOwnedFragmentProperties::ApplyRaw(
	const FAssetDocumentEditorSequenceFragmentContext& C, const IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction& T)
{
	if (Impl->bApplyAttempted || Impl->bInverseAttempted) return FragmentFailure(TEXT("SequenceOwnedFragment.AlreadyConsumed"));
	auto R = Impl->ValidateForward(C, Lease, Handles, T); if (!R.bValid) return R;
	// A late mutation between Prepare and Apply must not be overwritten.
	for (const auto& F : Impl->Fields)
	{
		FAssetDocumentEditorPropertyTokenView V; F.Token->TryGetView(V);
		FAssetDocumentEditorScopedPropertyValue Current(*V.Property);
		if (!GetFragmentValue(*Impl->Identity.Child.Get(), *V.Property, Current)
			|| !SameFragmentValue(*V.Property, Current, *F.Before))
			return FragmentFailure(TEXT("SequenceOwnedFragment.PreimageDrift"));
	}
	Impl->bApplyAttempted = true;
	for (const auto& F : Impl->Fields)
	{
		FAssetDocumentEditorPropertyTokenView V;
		if (!CurrentFragmentField(Impl->Identity, F, V))
			return FragmentFailure(TEXT("SequenceOwnedFragment.RawWriteFailed"));
		// Include every attempted setter in the inverse, even if it reports failure.
		++Impl->Written;
		if (!SetFragmentRaw(*Impl->Identity.Child.Get(), *V.Property, *F.Desired))
			return FragmentFailure(TEXT("SequenceOwnedFragment.RawWriteFailed"));
		FAssetDocumentEditorScopedPropertyValue Readback(*V.Property);
		if (!GetFragmentValue(*Impl->Identity.Child.Get(), *V.Property, Readback)
			|| !SameFragmentValue(*V.Property, Readback, *F.Desired))
			return FragmentFailure(TEXT("SequenceOwnedFragment.ReadbackFailed"));
	}
	return Impl->ValidateForward(C, Lease, Handles, T);
}

FAssetDocumentContractValidationResult FAssetDocumentEditorSequenceOwnedFragmentProperties::RestoreRaw(
	const FAssetDocumentEditorSequenceFragmentContext& C, IAssetDocumentAtomicTransaction& T)
{
	if (Impl->bInverseAttempted) return FragmentFailure(TEXT("SequenceOwnedFragment.AlreadyConsumed"));
	// Core revokes forward handles before invoking rollback actions. The exact
	// captured native preimage remains the inverse authority, not a revived lease.
	auto R = Impl->ValidateNative(C, T, true); if (!R.bValid) return R;
	Impl->bInverseAttempted = true;
	for (int32 I = Impl->Written - 1; I >= 0; --I)
	{
		const auto& F = Impl->Fields[I]; FAssetDocumentEditorPropertyTokenView V;
		if (!CurrentFragmentField(Impl->Identity, F, V) || !SetFragmentRaw(*Impl->Identity.Child.Get(), *V.Property, *F.Before))
			return FragmentFailure(TEXT("SequenceOwnedFragment.RawInverseFailed"));
		FAssetDocumentEditorScopedPropertyValue Readback(*V.Property);
		if (!GetFragmentValue(*Impl->Identity.Child.Get(), *V.Property, Readback)
			|| !SameFragmentValue(*V.Property, Readback, *F.Before))
			return FragmentFailure(TEXT("SequenceOwnedFragment.InverseReadbackFailed"));
	}
	R = Impl->ValidateNative(C, T, true);
	if (R.bValid) Impl->Fields.Reset();
	return R;
}

FAssetDocumentEditorSequenceFragmentReadResult ReadAssetDocumentEditorSequenceOwnedFragmentProperties(
	const FAssetDocumentEditorSequenceFragmentContext& C, const IAssetDocumentVerifiedHandleLease& Lease, IAssetDocumentHandleResolver& Handles,
	UObject& Child, TConstArrayView<FString> Names)
{
	FAssetDocumentEditorSequenceFragmentReadResult Out; Out.Properties.Kind = EAssetDocumentRuntimeValueKind::Object;
	FSequenceFragmentIdentity Identity; auto R = CaptureIdentity(C, Lease, Handles, Child, Identity);
	TArray<FSequenceFragmentField> Fields;
	if (R.bValid) R = AdmitFragmentFields(*Identity.ChildClass.Get(), Names, Fields);
	if (!R.bValid) { Out.Diagnostics = MoveTemp(R.Diagnostics); return Out; }
	for (const auto& F : Fields)
	{
		FAssetDocumentEditorPropertyTokenView V;
		if (!AcquireFragmentParent(C, Lease, Handles, false).Lease || !Identity.Current() || !CurrentFragmentField(Identity, F, V))
			{ Out.Diagnostics = FragmentFailure(TEXT("SequenceOwnedFragment.StaleField")).Diagnostics; return Out; }
		auto Read = ReadAssetDocumentEditorAdmittedProperty(Child, V);
		const int32 Index = Read.Value.ObjectKeys.IndexOfByKey(TEXT("value"));
		if (!Read.bSucceeded || !Read.Value.Elements.IsValidIndex(Index))
			{ Out.Diagnostics = MoveTemp(Read.Diagnostics); if (Out.Diagnostics.IsEmpty()) Out.Diagnostics = FragmentFailure(TEXT("SequenceOwnedFragment.ReadFailed")).Diagnostics; return Out; }
		Out.Properties.ObjectKeys.Add(F.Name); Out.Properties.Elements.Add(MoveTemp(Read.Value.Elements[Index]));
	}
	if (!AcquireFragmentParent(C, Lease, Handles, false).Lease || !Identity.Current()) { Out.Diagnostics = FragmentFailure(TEXT("SequenceOwnedFragment.StaleParticipant")).Diagnostics; return Out; }
	Out.bSucceeded = true;
	return Out;
}
