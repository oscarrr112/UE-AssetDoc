// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AnimAssets/AssetDocumentSkeletonRawEdits.h"

#include "Animation/AnimCurveMetadata.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMeshSocket.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Animation::SkeletonEdits
{
void RemoveSocket(USkeleton& Skeleton, USkeletalMeshSocket& Socket)
{
	const FName SocketName = Socket.SocketName;
	Skeleton.Modify();
	Skeleton.Sockets.Remove(&Socket);
	while (UObject* Object = Skeleton.PreviewAttachedAssetContainer.GetAttachedObjectByAttachName(SocketName))
	{
		Skeleton.Modify();
		Skeleton.PreviewAttachedAssetContainer.RemoveAttachedObject(Object, SocketName);
	}
}

bool SetSocketField(USkeletalMeshSocket& Socket, FName Field, TFunctionRef<void(USkeletalMeshSocket&)> Write)
{
	FProperty* Property = USkeletalMeshSocket::StaticClass()->FindPropertyByName(Field);
	if (Property == nullptr || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_EditConst))
	{
		return false;
	}
	Socket.SetFlags(RF_Transactional);
	Socket.Modify();
	Socket.PreEditChange(Property);
	Write(Socket);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Socket.PostEditChangeProperty(Event);
	return true;
}
namespace
{
// A USkeleton user data array property by name (AssetUserData or AssetUserDataEditorOnly) with its element type.
bool SkeletonUserDataArray(const TCHAR* Name, FArrayProperty*& OutProperty, const FObjectPropertyBase*& OutInner)
{
	OutProperty = FindFProperty<FArrayProperty>(USkeleton::StaticClass(), Name);
	OutInner = OutProperty != nullptr ? CastField<FObjectPropertyBase>(OutProperty->Inner) : nullptr;
	return OutInner != nullptr;
}

bool IsSkeletonCurveMetaData(const FObjectPropertyBase& Inner, FScriptArrayHelper& Array, int32 Index)
{
	if (!Array.IsValidIndex(Index))
	{
		return false;
	}
	const UObject* Element = Inner.GetObjectPropertyValue(Array.GetRawPtr(Index));
	return Element != nullptr && Element->IsA<UAnimCurveMetaData>();
}

// Moves the curve metadata element at FromIndex of one array to ToIndex (clamped) of the other, as two Details panel
// array edits (an element removed, one added).
bool MoveSkeletonUserDataElement(USkeleton& Skeleton, const TCHAR* FromName, int32 FromIndex, const TCHAR* ToName, int32 ToIndex)
{
	FArrayProperty* From = nullptr;
	FArrayProperty* To = nullptr;
	const FObjectPropertyBase* FromInner = nullptr;
	const FObjectPropertyBase* ToInner = nullptr;
	if (!SkeletonUserDataArray(FromName, From, FromInner) || !SkeletonUserDataArray(ToName, To, ToInner))
	{
		return false;
	}
	FScriptArrayHelper FromArray(From, From->ContainerPtrToValuePtr<void>(&Skeleton));
	if (!IsSkeletonCurveMetaData(*FromInner, FromArray, FromIndex))
	{
		return false;
	}
	UObject* Moved = FromInner->GetObjectPropertyValue(FromArray.GetRawPtr(FromIndex));
	Skeleton.Modify();

	Skeleton.PreEditChange(From);
	FromArray.RemoveValues(FromIndex, 1);
	FPropertyChangedEvent Removed(From, EPropertyChangeType::ArrayRemove);
	Skeleton.PostEditChangeProperty(Removed);

	FScriptArrayHelper ToArray(To, To->ContainerPtrToValuePtr<void>(&Skeleton));
	const int32 Slot = FMath::Clamp(ToIndex, 0, ToArray.Num());
	Skeleton.PreEditChange(To);
	ToArray.InsertValues(Slot, 1);
	ToInner->SetObjectPropertyValue(ToArray.GetRawPtr(Slot), Moved);
	FPropertyChangedEvent Added(To, EPropertyChangeType::ArrayAdd);
	Skeleton.PostEditChangeProperty(Added);
	return true;
}
} // namespace

int32 FindCurveMetaData(const USkeleton& Skeleton)
{
	FArrayProperty* Property = nullptr;
	const FObjectPropertyBase* Inner = nullptr;
	if (!SkeletonUserDataArray(TEXT("AssetUserData"), Property, Inner))
	{
		return INDEX_NONE;
	}
	FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(const_cast<USkeleton*>(&Skeleton)));
	for (int32 Index = 0; Index < Array.Num(); ++Index)
	{
		if (IsSkeletonCurveMetaData(*Inner, Array, Index))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool ParkCurveMetaData(USkeleton& Skeleton, int32 Index)
{
	FArrayProperty* Parking = nullptr;
	const FObjectPropertyBase* ParkingInner = nullptr;
	if (!SkeletonUserDataArray(TEXT("AssetUserDataEditorOnly"), Parking, ParkingInner))
	{
		return false;
	}
	const FScriptArrayHelper ParkingArray(Parking, Parking->ContainerPtrToValuePtr<void>(&Skeleton));
	return MoveSkeletonUserDataElement(Skeleton, TEXT("AssetUserData"), Index, TEXT("AssetUserDataEditorOnly"), ParkingArray.Num());
}

bool UnparkCurveMetaData(USkeleton& Skeleton, int32 Index)
{
	FArrayProperty* Parking = nullptr;
	const FObjectPropertyBase* ParkingInner = nullptr;
	if (!SkeletonUserDataArray(TEXT("AssetUserDataEditorOnly"), Parking, ParkingInner))
	{
		return false;
	}
	const FScriptArrayHelper ParkingArray(Parking, Parking->ContainerPtrToValuePtr<void>(&Skeleton));
	return MoveSkeletonUserDataElement(Skeleton, TEXT("AssetUserDataEditorOnly"), ParkingArray.Num() - 1, TEXT("AssetUserData"), Index);
}
} // namespace UE::AssetDocument::Animation::SkeletonEdits
