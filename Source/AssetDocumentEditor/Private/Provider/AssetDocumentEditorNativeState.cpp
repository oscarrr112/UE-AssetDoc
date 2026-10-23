// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorNativeState.h"

#include "AssetDocumentRollbackAction.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "PropertyHandle.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/PropertyOptional.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr const TCHAR* IdentityDiagnosticPath =
	TEXT("/EditorNativeState/Identity");
constexpr const TCHAR* PreimageDiagnosticPath =
	TEXT("/EditorNativeState/Preimage");
constexpr const TCHAR* RollbackDiagnosticPath =
	TEXT("/EditorNativeState/Rollback");

FAssetDocumentRuntimeDiagnostic MakeDiagnostic(
	const TCHAR* Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentContractValidationResult MakeValidationFailure(
	const TCHAR* Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.Diagnostics.Add(MakeDiagnostic(Path, Code, Message));
	return Result;
}

FAssetDocumentContractValidationResult MakeValidationSuccess()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentEditorTypedPreimageResult MakePreimageFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentEditorTypedPreimageResult Result;
	Result.Diagnostics.Add(MakeDiagnostic(
		PreimageDiagnosticPath, Code, Message));
	return Result;
}

bool IsEditorGameThread()
{
	return GIsEditor && IsInGameThread();
}

bool IsAdmittedObject(const UObject* Object)
{
	return IsValid(Object)
		&& !Object->IsTemplate()
		&& !Object->HasAnyFlags(
			RF_ClassDefaultObject
			| RF_ArchetypeObject
			| RF_BeginDestroyed
			| RF_FinishDestroyed);
}

bool IsUnsupportedObjectReferenceFamily(const FProperty& Property)
{
	if (Property.GetOwnerProperty() != &Property
		|| CastField<FArrayProperty>(&Property) != nullptr
		|| CastField<FSetProperty>(&Property) != nullptr
		|| CastField<FMapProperty>(&Property) != nullptr
		|| CastField<FOptionalProperty>(&Property) != nullptr
		|| CastField<FClassProperty>(&Property) != nullptr)
	{
		return true;
	}

	const FObjectPropertyBase* ObjectProperty =
		CastField<FObjectPropertyBase>(&Property);
	const UClass* ReferenceClass =
		ObjectProperty != nullptr ? ObjectProperty->PropertyClass : nullptr;
	return ReferenceClass != nullptr
		&& (ReferenceClass->IsChildOf(UClass::StaticClass())
			|| ReferenceClass->IsChildOf(UPackage::StaticClass())
			|| ReferenceClass->IsChildOf(UEdGraph::StaticClass())
			|| ReferenceClass->IsChildOf(UEdGraphNode::StaticClass()));
}

bool DoesGeneratorSelectExactOwner(
	const IPropertyRowGenerator& PropertyRows,
	const UObject& ExactOwner)
{
	const TArray<TWeakObjectPtr<UObject>>& SelectedObjects =
		PropertyRows.GetSelectedObjects();
	return SelectedObjects.Num() == 1
		&& SelectedObjects[0].Get() == &ExactOwner;
}

bool HasExactOuterObject(
	const IPropertyHandle& PropertyHandle,
	const UObject& ExactOwner)
{
	TArray<UObject*> OuterObjects;
	PropertyHandle.GetOuterObjects(OuterObjects);
	return OuterObjects.Num() == 1
		&& OuterObjects[0] == &ExactOwner;
}

struct FObjectReferencePreimageState;

void GatherMatchingCurrentHandles(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	const FObjectReferencePreimageState& State,
	TArray<TSharedPtr<IPropertyHandle>>& OutMatches);

struct FObjectReferencePreimageState
	: public TSharedFromThis<FObjectReferencePreimageState>
{
	FObjectReferencePreimageState(
		UObject& InAssetAnchor,
		UObject& InPropertyOwner,
		UPackage& InPackage,
		UObject* InOriginalReference,
		TSharedRef<IPropertyRowGenerator> InPropertyRows,
		FObjectPropertyBase& InProperty,
		FString InPropertyPath,
		uint64 InAssetTypeRevision,
		bool bInOriginalPackageDirty)
		: AssetAnchor(&InAssetAnchor)
		, PropertyOwner(&InPropertyOwner)
		, Package(&InPackage)
		, OriginalReference(InOriginalReference)
		, PropertyRows(MoveTemp(InPropertyRows))
		, Property(&InProperty)
		, PropertyPath(MoveTemp(InPropertyPath))
		, PropertyName(InProperty.GetFName())
		, PropertyClass(InProperty.PropertyClass)
		, AssetPath(InAssetAnchor.GetPathName())
		, PropertyOwnerPath(InPropertyOwner.GetPathName())
		, PackageName(InPackage.GetName())
		, OriginalReferencePath(
			InOriginalReference != nullptr
				? InOriginalReference->GetPathName()
				: FString())
		, AssetUniqueId(InAssetAnchor.GetUniqueID())
		, PropertyOwnerUniqueId(InPropertyOwner.GetUniqueID())
		, PackageUniqueId(InPackage.GetUniqueID())
		, OriginalReferenceUniqueId(
			InOriginalReference != nullptr
				? InOriginalReference->GetUniqueID()
				: 0)
		, AssetTypeRevision(InAssetTypeRevision)
		, bOriginalPackageDirty(bInOriginalPackageDirty)
	{
	}

	FAssetDocumentContractValidationResult Restore()
	{
		if (bConsumed)
		{
			return CachedResult;
		}
		bConsumed = true;
		CachedResult = RestoreOnce();
		if (CachedResult.bValid)
		{
			// Atomic retains actions until all inverses finish. A completed typed
			// preimage must not keep a later factory inverse's target alive.
			PropertyRows.Reset();
			OriginalReference.Reset();
			PropertyOwner.Reset();
			AssetAnchor.Reset();
			Package.Reset();
		}
		return CachedResult;
	}

	bool IsCurrentPropertyMatch(const IPropertyHandle& Handle) const
	{
		FProperty* CurrentProperty = Handle.GetProperty();
		const FObjectPropertyBase* CurrentObjectProperty =
			CastField<FObjectPropertyBase>(CurrentProperty);
		return Handle.IsValidHandle()
			&& Handle.IsEditable()
			&& CurrentProperty != nullptr
			&& !IsUnsupportedObjectReferenceFamily(*CurrentProperty)
			&& CurrentProperty == Property.Get()
			&& CurrentProperty->GetFName() == PropertyName
			&& Handle.GeneratePathToProperty() == PropertyPath
			&& CurrentObjectProperty != nullptr
			&& CurrentObjectProperty->PropertyClass == PropertyClass
			&& PropertyOwner.IsValid()
			&& HasExactOuterObject(Handle, *PropertyOwner.Get());
	}

	bool FindUniqueCurrentHandle(
		TSharedPtr<IPropertyHandle>& OutHandle) const
	{
		if (!PropertyOwner.IsValid() || !PropertyRows.IsValid()
			|| !DoesGeneratorSelectExactOwner(
				*PropertyRows, *PropertyOwner.Get()))
		{
			return false;
		}
		TArray<TSharedPtr<IPropertyHandle>> Matches;
		GatherMatchingCurrentHandles(
			PropertyRows->GetRootTreeNodes(), *this, Matches);
		if (Matches.Num() != 1)
		{
			return false;
		}
		OutHandle = Matches[0];
		return OutHandle.IsValid();
	}

	FAssetDocumentContractValidationResult RestoreOnce()
	{
		if (!IsEditorGameThread())
		{
			return MakeValidationFailure(
				RollbackDiagnosticPath,
				TEXT("EditorNativeState.RollbackInvalidExecutionContext"),
				TEXT("Typed rollback requires the Editor game thread."));
		}
		UObject* Asset = AssetAnchor.Get();
		UObject* Owner = PropertyOwner.Get();
		UPackage* OwningPackage = Package.Get();
		if (!IsAdmittedObject(Asset)
			|| !IsAdmittedObject(Owner)
			|| OwningPackage == nullptr
			|| Asset->GetPathName() != AssetPath
			|| Owner->GetPathName() != PropertyOwnerPath
			|| OwningPackage->GetName() != PackageName
			|| Asset->GetUniqueID() != AssetUniqueId
			|| Owner->GetUniqueID() != PropertyOwnerUniqueId
			|| OwningPackage->GetUniqueID() != PackageUniqueId
			|| static_cast<uint64>(Asset->GetUniqueID())
				!= AssetTypeRevision
			|| Asset->GetOutermost() != OwningPackage
			|| Owner->GetOutermost() != OwningPackage)
		{
			return MakeValidationFailure(
				RollbackDiagnosticPath,
				TEXT("EditorNativeState.RollbackIdentityMismatch"),
				TEXT("Typed rollback object or package identity changed."));
		}
		const UObject* const Original = OriginalReference.Get();
		if ((OriginalReferenceUniqueId == 0 && Original != nullptr)
			|| (OriginalReferenceUniqueId != 0
				&& (!IsValid(Original)
					|| Original->GetUniqueID()
						!= OriginalReferenceUniqueId
					|| Original->GetPathName()
						!= OriginalReferencePath)))
		{
			return MakeValidationFailure(
				RollbackDiagnosticPath,
				TEXT("EditorNativeState.RollbackReferenceIdentityMismatch"),
				TEXT("The captured object-reference identity changed."));
		}

		TSharedPtr<IPropertyHandle> RestoreHandle;
		if (!FindUniqueCurrentHandle(RestoreHandle))
		{
			return MakeValidationFailure(
				RollbackDiagnosticPath,
				TEXT("EditorNativeState.RollbackPropertyHandleUnavailable"),
				TEXT("The exact current object-reference row is unavailable."));
		}
		if (RestoreHandle->SetValue(
			Original,
			EPropertyValueSetFlags::NotTransactable)
			!= FPropertyAccess::Success)
		{
			return MakeValidationFailure(
				RollbackDiagnosticPath,
				TEXT("EditorNativeState.RollbackPropertyWriteFailed"),
				TEXT("The typed object-reference restore failed."));
		}

		TSharedPtr<IPropertyHandle> ReadbackHandle;
		if (!FindUniqueCurrentHandle(ReadbackHandle))
		{
			return MakeValidationFailure(
				RollbackDiagnosticPath,
				TEXT("EditorNativeState.RollbackPropertyHandleUnavailable"),
				TEXT("The exact object-reference row was lost after restore."));
		}
		const UObject* Readback = nullptr;
		if (ReadbackHandle->GetValue(Readback) != FPropertyAccess::Success
			|| Readback != Original)
		{
			return MakeValidationFailure(
				RollbackDiagnosticPath,
				TEXT("EditorNativeState.RollbackPropertyReadbackMismatch"),
				TEXT("The typed object-reference restore did not read back exactly."));
		}

		OwningPackage->SetDirtyFlag(bOriginalPackageDirty);
		if (OwningPackage->IsDirty() != bOriginalPackageDirty)
		{
			return MakeValidationFailure(
				RollbackDiagnosticPath,
				TEXT("EditorNativeState.RollbackDirtyStateRestoreFailed"),
				TEXT("The owning package dirty preimage did not restore exactly."));
		}
		return MakeValidationSuccess();
	}

	TStrongObjectPtr<UObject> AssetAnchor;
	TStrongObjectPtr<UObject> PropertyOwner;
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UObject> OriginalReference;
	TSharedPtr<IPropertyRowGenerator> PropertyRows;
	TFieldPath<FProperty> Property;
	FString PropertyPath;
	FName PropertyName;
	TWeakObjectPtr<UClass> PropertyClass;
	FString AssetPath;
	FString PropertyOwnerPath;
	FString PackageName;
	FString OriginalReferencePath;
	int32 AssetUniqueId = 0;
	int32 PropertyOwnerUniqueId = 0;
	int32 PackageUniqueId = 0;
	int32 OriginalReferenceUniqueId = 0;
	uint64 AssetTypeRevision = 0;
	bool bOriginalPackageDirty = false;
	bool bConsumed = false;
	FAssetDocumentContractValidationResult CachedResult;
};

void GatherMatchingCurrentHandles(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	const FObjectReferencePreimageState& State,
	TArray<TSharedPtr<IPropertyHandle>>& OutMatches)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle =
			Node->CreatePropertyHandle();
		if (Handle.IsValid() && State.IsCurrentPropertyMatch(*Handle))
		{
			OutMatches.Add(Handle);
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		GatherMatchingCurrentHandles(Children, State, OutMatches);
	}
}

class FObjectReferencePreimageRollbackAction final
	: public IAssetDocumentRollbackAction
{
public:
	FObjectReferencePreimageRollbackAction(
		FString InComponentId,
		TSharedRef<FObjectReferencePreimageState> InState)
		: ComponentId(MoveTemp(InComponentId))
		, State(MoveTemp(InState))
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		(void)StableFailureCode;
		return State->Restore();
	}

private:
	FString ComponentId;
	TSharedRef<FObjectReferencePreimageState> State;
};
} // namespace

FAssetDocumentEditorAssetIdentityView::FAssetDocumentEditorAssetIdentityView(
	UObject& InObject,
	FStringView InCanonicalObjectPath,
	uint64 InTypeRevision,
	const FGuid& InRequestId,
	const FGuid& InTransactionId,
	uint64 InRuntimeRevision,
	EAssetDocumentHandleAccess InAccess)
	: Object(&InObject)
	, CanonicalObjectPath(InCanonicalObjectPath)
	, TypeRevision(InTypeRevision)
	, RequestId(InRequestId)
	, TransactionId(InTransactionId)
	, RuntimeRevision(InRuntimeRevision)
	, Access(InAccess)
{
}

UObject& FAssetDocumentEditorAssetIdentityView::GetObject() const
{
	check(Object != nullptr);
	return *Object;
}

FStringView
FAssetDocumentEditorAssetIdentityView::GetCanonicalObjectPath() const
{
	return CanonicalObjectPath;
}

uint64 FAssetDocumentEditorAssetIdentityView::GetTypeRevision() const
{
	return TypeRevision;
}

FAssetDocumentContractValidationResult
WithAssetDocumentEditorAssetIdentity(
	const IAssetDocumentVerifiedHandleLease& Lease,
	FAssetDocumentEditorAssetIdentityConsumer Consumer)
{
	if (!IsEditorGameThread())
	{
		return MakeValidationFailure(
			IdentityDiagnosticPath,
			TEXT("EditorNativeState.InvalidExecutionContext"),
			TEXT("Native Asset identity requires the Editor game thread."));
	}
	const FAssetDocumentOpaqueHandle& Handle = Lease.GetHandle();
	if (Handle.Kind != EAssetDocumentHandleKind::Asset)
	{
		return MakeValidationFailure(
			IdentityDiagnosticPath,
			TEXT("EditorNativeState.AssetHandleKindRequired"),
			TEXT("Native Asset identity requires an Asset handle lease."));
	}
	IAssetDocumentNativeHandlePayload& Payload = Lease.GetPayloadChecked();
	if (Payload.GetPayloadTypeId()
		!= UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId)
	{
		return MakeValidationFailure(
			IdentityDiagnosticPath,
			TEXT("EditorNativeState.AssetPayloadTypeMismatch"),
			TEXT("The verified lease is not backed by the Editor Asset payload family."));
	}
	if (!Payload.IsAlive())
	{
		return MakeValidationFailure(
			IdentityDiagnosticPath,
			TEXT("EditorNativeState.AssetPayloadNotAlive"),
			TEXT("The verified Editor Asset payload is no longer alive."));
	}
	UE::AssetDocument::EditorPrimitive::INativeIdentityPayload* IdentityPayload =
		UE::AssetDocument::EditorPrimitive::TryGetNativeIdentityPayload(
			Payload,
			UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId);
	UE::AssetDocument::EditorPrimitive::FObjectIdentityView Identity;
	if (IdentityPayload == nullptr
		|| !IdentityPayload->TryGetObjectIdentity(Identity))
	{
		return MakeValidationFailure(
			IdentityDiagnosticPath,
			TEXT("EditorNativeState.AssetPayloadNotAlive"),
			TEXT("The verified Editor Asset payload cannot supply live identity."));
	}
	if (!IsAdmittedObject(Identity.Object))
	{
		return MakeValidationFailure(
			IdentityDiagnosticPath,
			TEXT("EditorNativeState.AssetObjectInvalid"),
			TEXT("The Editor Asset identity is not an admitted live object."));
	}
	if (Identity.CanonicalObjectPath.IsEmpty()
		|| Identity.Object->GetPathName()
			!= Identity.CanonicalObjectPath)
	{
		return MakeValidationFailure(
			IdentityDiagnosticPath,
			TEXT("EditorNativeState.AssetIdentityMismatch"),
			TEXT("The Editor Asset canonical identity changed."));
	}
	if (Identity.TypeRevision == 0)
	{
		return MakeValidationFailure(
			IdentityDiagnosticPath,
			TEXT("EditorNativeState.AssetTypeRevisionInvalid"),
			TEXT("The Editor Asset type revision must be non-zero."));
	}

	const FAssetDocumentEditorAssetIdentityView View(
		*Identity.Object,
		Identity.CanonicalObjectPath,
		Identity.TypeRevision,
		Handle.RequestId,
		Handle.TransactionId,
		Handle.RuntimeRevision,
		Handle.Access);
	return Consumer(View);
}

FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
	const FAssetDocumentEditorAssetIdentityView& AssetIdentity,
	UObject& ExactPropertyOwner,
	TSharedRef<IPropertyRowGenerator> PropertyRows,
	TSharedRef<IPropertyHandle> PropertyHandle,
	TConstArrayView<FString> RollbackComponentIds,
	IAssetDocumentAtomicTransaction& Transaction)
{
	if (!IsEditorGameThread())
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.InvalidExecutionContext"),
			TEXT("Typed preimage capture requires the Editor game thread."));
	}
	const EAssetDocumentAtomicState State = Transaction.GetState();
	if ((State != EAssetDocumentAtomicState::Live
			&& State != EAssetDocumentAtomicState::Mutating)
		|| !Transaction.GetTransactionId().IsValid())
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.TransactionStateInvalid"),
			TEXT("Typed preimage capture requires a live active transaction."));
	}
	const FAssetDocumentMutationToken& Token =
		Transaction.GetMutationToken();
	if (AssetIdentity.Access != EAssetDocumentHandleAccess::Mutable
		|| AssetIdentity.RequestId != Token.RequestId
		|| AssetIdentity.TransactionId != Token.TransactionId
		|| AssetIdentity.TransactionId != Transaction.GetTransactionId()
		|| AssetIdentity.RuntimeRevision != Token.RuntimeRevision)
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.TransactionBindingMismatch"),
			TEXT("The verified mutable lease does not belong to the active transaction."));
	}
	UObject* Asset = AssetIdentity.Object;
	if (!IsAdmittedObject(Asset)
		|| Asset->GetPathName() != AssetIdentity.CanonicalObjectPath
		|| static_cast<uint64>(Asset->GetUniqueID())
			!= AssetIdentity.TypeRevision)
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.AssetIdentityStale"),
			TEXT("The verified Asset identity changed before preimage capture."));
	}
	if (!IsAdmittedObject(&ExactPropertyOwner)
		|| ExactPropertyOwner.GetOutermost() == nullptr
		|| Asset->GetOutermost() != ExactPropertyOwner.GetOutermost())
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.PropertyOwnerInvalid"),
			TEXT("The exact property owner is invalid or outside the Asset package."));
	}
	if (!DoesGeneratorSelectExactOwner(*PropertyRows, ExactPropertyOwner))
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.PropertyRowOwnerMismatch"),
			TEXT("The property-row generator must select only the exact property owner."));
	}
	const TSharedPtr<IPropertyHandle> PropertyHandlePtr = PropertyHandle;
	if (!PropertyHandle->IsValidHandle()
		|| !PropertyHandle->IsEditable()
		|| !PropertyRows->FindTreeNode(PropertyHandlePtr).IsValid())
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.PropertyHandleUnavailable"),
			TEXT("The exact editable property handle is not owned by the supplied rows."));
	}
	if (!HasExactOuterObject(*PropertyHandle, ExactPropertyOwner))
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.PropertyOuterMismatch"),
			TEXT("The property handle must address exactly one matching outer object."));
	}
	FProperty* Property = PropertyHandle->GetProperty();
	if (Property != nullptr
		&& IsUnsupportedObjectReferenceFamily(*Property))
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.ObjectReferencePropertyRequired"),
			TEXT("Container, optional, Class, Package, Graph, and Node reference families are unsupported."));
	}
	FObjectPropertyBase* ObjectProperty =
		CastField<FObjectPropertyBase>(Property);
	if (ObjectProperty == nullptr
		|| ObjectProperty->GetOwnerClass() == nullptr
		|| !ExactPropertyOwner.IsA(ObjectProperty->GetOwnerClass()))
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.ObjectReferencePropertyRequired"),
			TEXT("Typed preimage capture supports only one UObject-reference property."));
	}
	UObject* OriginalReference = nullptr;
	if (PropertyHandle->GetValue(OriginalReference)
		!= FPropertyAccess::Success
		|| (OriginalReference != nullptr && !IsValid(OriginalReference)))
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.PropertyReadFailed"),
			TEXT("The exact object-reference preimage could not be read."));
	}
	TSet<FString> UniqueComponentIds;
	if (RollbackComponentIds.IsEmpty())
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.RollbackComponentInvalid"),
			TEXT("Typed preimage capture requires rollback component IDs."));
	}
	for (const FString& ComponentId : RollbackComponentIds)
	{
		if (ComponentId.IsEmpty()
			|| UniqueComponentIds.Contains(ComponentId))
		{
			return MakePreimageFailure(
				TEXT("EditorNativeState.RollbackComponentInvalid"),
				TEXT("Rollback component IDs must be non-empty and unique."));
		}
		UniqueComponentIds.Add(ComponentId);
	}
	const FString PropertyPath = PropertyHandle->GeneratePathToProperty();
	if (PropertyPath.IsEmpty())
	{
		return MakePreimageFailure(
			TEXT("EditorNativeState.PropertyHandleUnavailable"),
			TEXT("The exact property handle has no stable generated path."));
	}
	UPackage* Package = ExactPropertyOwner.GetOutermost();
	check(Package != nullptr);
	const TSharedRef<FObjectReferencePreimageState> Preimage =
		MakeShared<FObjectReferencePreimageState>(
			*Asset,
			ExactPropertyOwner,
			*Package,
			OriginalReference,
			MoveTemp(PropertyRows),
			*ObjectProperty,
			PropertyPath,
			AssetIdentity.TypeRevision,
			Package->IsDirty());
	for (const FString& ComponentId : RollbackComponentIds)
	{
		const FAssetDocumentContractValidationResult Enlisted =
			Transaction.EnlistRollbackAction(
				MakeShared<FObjectReferencePreimageRollbackAction>(
					ComponentId, Preimage));
		if (!Enlisted.bValid)
		{
			return MakePreimageFailure(
				TEXT("EditorNativeState.RollbackActionEnlistmentFailed"),
				TEXT("The active transaction rejected a typed rollback action."));
		}
	}

	FAssetDocumentEditorTypedPreimageResult Result;
	Result.bSucceeded = true;
	return Result;
}
