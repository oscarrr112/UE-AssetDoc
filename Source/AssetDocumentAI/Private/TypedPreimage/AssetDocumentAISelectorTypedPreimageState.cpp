// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "TypedPreimage/AssetDocumentAISelectorTypedPreimageInternal.h"

#include "TypedPreimage/AssetDocumentAITypedPreimageInternal.h"

#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "PropertyHandle.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::AI::TypedPreimage::Selector
{
using namespace UE::AssetDocument::AI::TypedPreimage;

constexpr EPropertyValueSetFlags::Type RestoreFlags =
	EPropertyValueSetFlags::NotTransactable;

bool DoesGeneratorSelectExactOwner(
	const IPropertyRowGenerator& Rows,
	const UObject& Owner)
{
	const TArray<TWeakObjectPtr<UObject>>& Selected = Rows.GetSelectedObjects();
	return Selected.Num() == 1 && Selected[0].Get() == &Owner;
}

bool HasExactOuterObject(
	const IPropertyHandle& Handle,
	const UObject& Owner)
{
	TArray<UObject*> Outers;
	Handle.GetOuterObjects(Outers);
	return Outers.Num() == 1 && Outers[0] == &Owner;
}

struct FSelectorHandles
{
	TSharedPtr<IPropertyHandle> Selector;
	TSharedPtr<IPropertyHandle> Name;
	TSharedPtr<IPropertyHandle> Type;
	TSharedPtr<IPropertyHandle> Id;
	TSharedPtr<IPropertyHandle> NoneAllowed;
};

bool ValidateSelectorChildren(FSelectorHandles& Handles)
{
	if (!Handles.Selector.IsValid())
	{
		return false;
	}
	Handles.Name = Handles.Selector->GetChildHandle(TEXT("SelectedKeyName"));
	Handles.Type = Handles.Selector->GetChildHandle(TEXT("SelectedKeyType"));
	Handles.Id = Handles.Selector->GetChildHandle(TEXT("SelectedKeyID"));
	Handles.NoneAllowed =
		Handles.Selector->GetChildHandle(TEXT("bNoneIsAllowedValue"));
	if (!Handles.Name.IsValid()
		|| !Handles.Type.IsValid()
		|| !Handles.Id.IsValid()
		|| !Handles.NoneAllowed.IsValid())
	{
		return false;
	}
	const FProperty* ExpectedName = FindFProperty<FNameProperty>(
		FBlackboardKeySelector::StaticStruct(), TEXT("SelectedKeyName"));
	const FProperty* ExpectedType = FindFProperty<FClassProperty>(
		FBlackboardKeySelector::StaticStruct(), TEXT("SelectedKeyType"));
	const FProperty* ExpectedId = FindFProperty<FIntProperty>(
		FBlackboardKeySelector::StaticStruct(), TEXT("SelectedKeyID"));
	const FProperty* ExpectedNone = FindFProperty<FBoolProperty>(
		FBlackboardKeySelector::StaticStruct(), TEXT("bNoneIsAllowedValue"));
	const TSharedPtr<IPropertyHandle> NameParent =
		Handles.Name->GetParentHandle();
	const TSharedPtr<IPropertyHandle> TypeParent =
		Handles.Type->GetParentHandle();
	const TSharedPtr<IPropertyHandle> IdParent =
		Handles.Id->GetParentHandle();
	const TSharedPtr<IPropertyHandle> NoneParent =
		Handles.NoneAllowed->GetParentHandle();
	return Handles.Name->GetProperty() == ExpectedName
		&& Handles.Type->GetProperty() == ExpectedType
		&& Handles.Id->GetProperty() == ExpectedId
		&& Handles.NoneAllowed->GetProperty() == ExpectedNone
		&& NameParent.IsValid()
		&& TypeParent.IsValid()
		&& IdParent.IsValid()
		&& NoneParent.IsValid()
		&& NameParent->GetProperty() == Handles.Selector->GetProperty()
		&& TypeParent->GetProperty() == Handles.Selector->GetProperty()
		&& IdParent->GetProperty() == Handles.Selector->GetProperty()
		&& NoneParent->GetProperty() == Handles.Selector->GetProperty();
}

struct FSelectorRollbackState final : public FAliasRollbackState
{
	FSelectorRollbackState(
		UObject& InAsset,
		uint64 InAssetTypeRevision,
		UObject& InOwner,
		UPackage& InPackage,
		TSharedRef<IPropertyRowGenerator> InRows,
		FStructProperty& InSelectorProperty,
		FString InSelectorPropertyPath,
		FName InSelectedName,
		UClass* InSelectedType,
		int32 InSelectedId,
		TArray<TStrongObjectPtr<UBlackboardKeyType>> InAllowedTypes,
		bool bInNoneAllowed,
		bool bInPackageDirty)
		: Asset(&InAsset)
		, Owner(&InOwner)
		, Package(&InPackage)
		, Rows(MoveTemp(InRows))
		, SelectorProperty(&InSelectorProperty)
		, SelectorPropertyPath(MoveTemp(InSelectorPropertyPath))
		, SelectorPropertyName(InSelectorProperty.GetFName())
		, AssetPath(InAsset.GetPathName())
		, OwnerPath(InOwner.GetPathName())
		, PackageName(InPackage.GetName())
		, AssetUniqueId(InAsset.GetUniqueID())
		, OwnerUniqueId(InOwner.GetUniqueID())
		, PackageUniqueId(InPackage.GetUniqueID())
		, AssetTypeRevision(InAssetTypeRevision)
		, SelectedName(InSelectedName)
		, SelectedType(InSelectedType)
		, SelectedId(InSelectedId)
		, AllowedTypes(MoveTemp(InAllowedTypes))
		, bNoneAllowed(bInNoneAllowed)
		, bPackageDirty(bInPackageDirty)
	{
	}

	bool MatchesSelectorHandle(const IPropertyHandle& Handle) const
	{
		FProperty* Property = Handle.GetProperty();
		const FStructProperty* StructProperty =
			CastField<FStructProperty>(Property);
		return Handle.IsValidHandle()
			&& Handle.IsEditable()
			&& Property != nullptr
			&& Property == SelectorProperty.Get()
			&& Property->GetOwnerProperty() == Property
			&& Property->GetFName() == SelectorPropertyName
			&& Handle.GeneratePathToProperty() == SelectorPropertyPath
			&& StructProperty != nullptr
			&& StructProperty->Struct == FBlackboardKeySelector::StaticStruct()
			&& Owner.IsValid()
			&& HasExactOuterObject(Handle, *Owner.Get());
	}

	bool Rebind(FSelectorHandles& OutHandles) const
	{
		if (NestedRebind)
		{
			OutHandles.Selector=NestedRebind();
			return OutHandles.Selector.IsValid() && NestedBinding && NestedBinding(*OutHandles.Selector)
				&& ValidateSelectorChildren(OutHandles);
		}
		if (!Owner.IsValid()
			|| !DoesGeneratorSelectExactOwner(*Rows, *Owner.Get()))
		{
			return false;
		}
		TArray<TSharedPtr<IPropertyHandle>> Matches;
		TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather =
			[&](const TArray<TSharedRef<IDetailTreeNode>>& Nodes)
			{
				for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
				{
					const TSharedPtr<IPropertyHandle> Handle =
						Node->CreatePropertyHandle();
					if (Handle.IsValid() && MatchesSelectorHandle(*Handle))
					{
						Matches.Add(Handle);
					}
					TArray<TSharedRef<IDetailTreeNode>> Children;
					Node->GetChildren(Children, true);
					Gather(Children);
				}
			};
		Gather(Rows->GetRootTreeNodes());
		if (Matches.Num() != 1)
		{
			return false;
		}
		OutHandles.Selector = Matches[0];
		return ValidateSelectorChildren(OutHandles);
	}

	bool InvariantsMatch(const FSelectorHandles& Handles) const
	{
		const UObject* OwnerObject = Owner.Get();
		const FStructProperty* StructProperty =
			CastField<FStructProperty>(SelectorProperty.Get());
		if (OwnerObject == nullptr || StructProperty == nullptr)
		{
			return false;
		}
		void* Address=nullptr;
		if (NestedRebind && Handles.Selector->GetValueData(Address)!=FPropertyAccess::Success) return false;
		const FBlackboardKeySelector* Selector = NestedRebind ? static_cast<const FBlackboardKeySelector*>(Address)
			: StructProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(OwnerObject);
		if (Selector == nullptr
			|| Selector->AllowedTypes.Num() != AllowedTypes.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < AllowedTypes.Num(); ++Index)
		{
			if (Selector->AllowedTypes[Index].Get() != AllowedTypes[Index].Get())
			{
				return false;
			}
		}
		bool CurrentNoneAllowed = false;
		return Handles.NoneAllowed->GetValue(CurrentNoneAllowed)
				== FPropertyAccess::Success
			&& CurrentNoneAllowed == bNoneAllowed;
	}

public:
	TFunction<TSharedPtr<IPropertyHandle>()> NestedRebind;
	TFunction<bool(IPropertyHandle&)> NestedBinding;
protected:
	virtual FAssetDocumentContractValidationResult RestoreOnce() override
	{
		if (!IsEditorGameThread())
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.InvalidExecutionContext"),
				TEXT("BehaviorTree selector rollback requires the Editor game thread."));
		}
		UObject* CurrentAsset = Asset.Get();
		UObject* CurrentOwner = Owner.Get();
		UPackage* CurrentPackage = Package.Get();
		if (!IsAdmittedObject(CurrentAsset)
			|| !IsAdmittedObject(CurrentOwner)
			|| CurrentPackage == nullptr
			|| CurrentAsset->GetPathName() != AssetPath
			|| CurrentOwner->GetPathName() != OwnerPath
			|| CurrentPackage->GetName() != PackageName
			|| CurrentAsset->GetUniqueID() != AssetUniqueId
			|| CurrentOwner->GetUniqueID() != OwnerUniqueId
			|| CurrentPackage->GetUniqueID() != PackageUniqueId
			|| static_cast<uint64>(CurrentAsset->GetUniqueID())
				!= AssetTypeRevision
			|| CurrentAsset->GetOutermost() != CurrentPackage
			|| CurrentOwner->GetOutermost() != CurrentPackage)
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackIdentityMismatch"),
				TEXT("The selector Asset, owner, or package identity changed."));
		}

		FSelectorHandles Handles;
		if (!Rebind(Handles))
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackFieldUnavailable"),
				TEXT("The exact current selector fields are unavailable."));
		}
		if (!InvariantsMatch(Handles))
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackInvariantMismatch"),
				TEXT("The selector AllowedTypes or none-allowed invariant changed."));
		}
		const UObject* SelectedTypeObject = SelectedType.Get();
		if (Handles.Name->SetValue(SelectedName, RestoreFlags)
			!= FPropertyAccess::Success)
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackWriteFailed"),
				TEXT("A typed selector-field restore failed."));
		}
		FSelectorHandles AfterName;
		if (!Rebind(AfterName)
			|| AfterName.Type->SetValue(SelectedTypeObject, RestoreFlags)
				!= FPropertyAccess::Success)
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackWriteFailed"),
				TEXT("SelectedKeyType could not be restored through a current row."));
		}
		FSelectorHandles AfterType;
		if (!Rebind(AfterType)
			|| AfterType.Id->SetValue(SelectedId, RestoreFlags)
				!= FPropertyAccess::Success)
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackWriteFailed"),
				TEXT("SelectedKeyID could not be restored through a current row."));
		}

		FSelectorHandles Readback;
		if (!Rebind(Readback))
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackFieldUnavailable"),
				TEXT("The selector fields were lost after restore."));
		}
		FName CurrentName;
		UObject* CurrentType = nullptr;
		int32 CurrentId = INDEX_NONE;
		if (Readback.Name->GetValue(CurrentName) != FPropertyAccess::Success
			|| Readback.Type->GetValue(CurrentType) != FPropertyAccess::Success
			|| Readback.Id->GetValue(CurrentId) != FPropertyAccess::Success
			|| CurrentName != SelectedName
			|| CurrentType != SelectedType.Get()
			|| CurrentId != SelectedId)
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackReadbackMismatch"),
				TEXT("The exact stale selector triple did not read back."));
		}
		if (!InvariantsMatch(Readback))
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackInvariantMismatch"),
				TEXT("The selector invariants changed during rollback."));
		}
		CurrentPackage->SetDirtyFlag(bPackageDirty);
		if (CurrentPackage->IsDirty() != bPackageDirty)
		{
			return MakeValidationFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorRollbackReadbackMismatch"),
				TEXT("The selector package dirty preimage did not restore."));
		}
		return MakeValidationSuccess();
	}

private:
	TStrongObjectPtr<UObject> Asset;
	TStrongObjectPtr<UObject> Owner;
	TStrongObjectPtr<UPackage> Package;
	TSharedRef<IPropertyRowGenerator> Rows;
	TFieldPath<FStructProperty> SelectorProperty;
	FString SelectorPropertyPath;
	FName SelectorPropertyName;
	FString AssetPath;
	FString OwnerPath;
	FString PackageName;
	int32 AssetUniqueId = 0;
	int32 OwnerUniqueId = 0;
	int32 PackageUniqueId = 0;
	uint64 AssetTypeRevision = 0;
	FName SelectedName;
	TStrongObjectPtr<UClass> SelectedType;
	int32 SelectedId = INDEX_NONE;
	TArray<TStrongObjectPtr<UBlackboardKeyType>> AllowedTypes;
	bool bNoneAllowed = false;
	bool bPackageDirty = false;
};
FAssetDocumentEditorTypedPreimageResult CaptureSelectorState(
 const FAssetDocumentEditorAssetIdentityView& Identity,
 UObject& ExactPropertyOwner,
 TSharedRef<IPropertyRowGenerator> PropertyRows,
 TSharedRef<IPropertyHandle> SelectorPropertyHandle,
 TSharedPtr<FAliasRollbackState>& OutState,
 TFunction<TSharedPtr<IPropertyHandle>()> NestedRebind,
 TFunction<bool(IPropertyHandle&)> NestedBinding)
{
 FAssetDocumentEditorTypedPreimageResult Result;
	UObject& Asset = Identity.GetObject();
	if (!IsAdmittedObject(&ExactPropertyOwner)
		|| ExactPropertyOwner.GetOutermost() == nullptr
		|| Asset.GetOutermost()
			!= ExactPropertyOwner.GetOutermost()
		|| !DoesGeneratorSelectExactOwner(
			*PropertyRows, ExactPropertyOwner))
	{
		Result = MakePreimageFailure(
			SelectorPath,
			TEXT("AITypedPreimage.BlackboardSelectorRequired"),
			TEXT("A live same-package selector owner and exact rows are required."));
		return Result;
	}
	const TSharedPtr<IPropertyHandle> SelectorPtr =
		SelectorPropertyHandle;
	FStructProperty* SelectorProperty = CastField<FStructProperty>(
		SelectorPropertyHandle->GetProperty());
	if (!SelectorPropertyHandle->IsValidHandle()
		|| !SelectorPropertyHandle->IsEditable()
		|| (!NestedRebind && !PropertyRows->FindTreeNode(SelectorPtr).IsValid())
		|| (!NestedRebind && !HasExactOuterObject(*SelectorPropertyHandle, ExactPropertyOwner))
		|| (NestedRebind && (!NestedBinding || !NestedBinding(*SelectorPropertyHandle)))
		|| SelectorProperty == nullptr
		|| (!NestedRebind && (SelectorProperty->GetOwnerProperty()!=SelectorProperty
			|| SelectorProperty->GetOwnerClass()==nullptr || !ExactPropertyOwner.IsA(SelectorProperty->GetOwnerClass())))
		|| SelectorProperty->Struct
			!= FBlackboardKeySelector::StaticStruct()
		|| (!NestedRebind && !SelectorProperty->HasAnyPropertyFlags(CPF_Edit)))
	{
		Result = MakePreimageFailure(
			SelectorPath,
			TEXT("AITypedPreimage.BlackboardSelectorRequired"),
			TEXT("An editable top-level FBlackboardKeySelector row is required."));
		return Result;
	}
	const FString PropertyPath =
		SelectorPropertyHandle->GeneratePathToProperty();
	FSelectorHandles Handles;
	Handles.Selector = SelectorPropertyHandle;
	if (PropertyPath.IsEmpty() || !ValidateSelectorChildren(Handles))
	{
		Result = MakePreimageFailure(
			SelectorPath,
			TEXT("AITypedPreimage.BlackboardSelectorFieldUnavailable"),
			TEXT("The exact selector child fields are unavailable."));
		return Result;
	}

	FName SelectedName;
	UObject* SelectedTypeObject = nullptr;
	int32 SelectedId = INDEX_NONE;
	bool bNoneAllowed = false;
	if (Handles.Name->GetValue(SelectedName)
			!= FPropertyAccess::Success
		|| Handles.Type->GetValue(SelectedTypeObject)
			!= FPropertyAccess::Success
		|| Handles.Id->GetValue(SelectedId)
			!= FPropertyAccess::Success
		|| Handles.NoneAllowed->GetValue(bNoneAllowed)
			!= FPropertyAccess::Success)
	{
		Result = MakePreimageFailure(
			SelectorPath,
			TEXT("AITypedPreimage.BlackboardSelectorReadFailed"),
			TEXT("The exact selector preimage could not be read."));
		return Result;
	}
	UClass* SelectedType = Cast<UClass>(SelectedTypeObject);
	if (SelectedTypeObject != nullptr && SelectedType == nullptr)
	{
		Result = MakePreimageFailure(
			SelectorPath,
			TEXT("AITypedPreimage.BlackboardSelectorReadFailed"),
			TEXT("SelectedKeyType is not a UClass identity."));
		return Result;
	}
	void* Address=nullptr;
	if (NestedRebind && SelectorPropertyHandle->GetValueData(Address)!=FPropertyAccess::Success)
		return MakePreimageFailure(SelectorPath,TEXT("AITypedPreimage.BlackboardSelectorReadFailed"),TEXT("The nested selector storage is unavailable."));
	const FBlackboardKeySelector* Selector = NestedRebind ? static_cast<const FBlackboardKeySelector*>(Address)
		: SelectorProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(&ExactPropertyOwner);
	if (!Selector) return MakePreimageFailure(SelectorPath,TEXT("AITypedPreimage.BlackboardSelectorReadFailed"),TEXT("The selector storage is absent."));
	TArray<TStrongObjectPtr<UBlackboardKeyType>> AllowedTypes;
	for (UBlackboardKeyType* Allowed : Selector->AllowedTypes)
	{
		if (!IsAdmittedObject(Allowed))
		{
			Result = MakePreimageFailure(
				SelectorPath,
				TEXT("AITypedPreimage.BlackboardSelectorReadFailed"),
				TEXT("A selector AllowedTypes identity is invalid."));
			return Result;
		}
		AllowedTypes.Emplace(Allowed);
	}
	UPackage* Package = ExactPropertyOwner.GetOutermost();
	check(Package != nullptr);
	auto State =
		MakeShared<FSelectorRollbackState>(
			Asset,
			Identity.GetTypeRevision(),
			ExactPropertyOwner,
			*Package,
			MoveTemp(PropertyRows),
			*SelectorProperty,
			PropertyPath,
			SelectedName,
			SelectedType,
			SelectedId,
			MoveTemp(AllowedTypes),
			bNoneAllowed,
			Package->IsDirty());
	State->NestedRebind=MoveTemp(NestedRebind);State->NestedBinding=MoveTemp(NestedBinding);OutState=MoveTemp(State);

	return MakePreimageSuccess();
}
} // namespace UE::AssetDocument::AI::TypedPreimage::Selector
