// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AnimAssets/AssetDocumentSkeletonRawEdits.h"

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
} // namespace UE::AssetDocument::Animation::SkeletonEdits
