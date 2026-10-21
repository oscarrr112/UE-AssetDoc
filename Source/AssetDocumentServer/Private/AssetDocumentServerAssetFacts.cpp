// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentServerAssetFacts.h"

#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequenceBase.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"

namespace
{
// Centimetres and seconds to 0.01 / 0.0001: what placement and timing need, without float noise.
double Round(double Value, double Step)
{
	return FMath::RoundToDouble(Value / Step) * Step;
}

TSharedPtr<FJsonValue> Vector(const FVector& Value)
{
	return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(Round(Value.X, 0.01)),
		MakeShared<FJsonValueNumber>(Round(Value.Y, 0.01)), MakeShared<FJsonValueNumber>(Round(Value.Z, 0.01))});
}

TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
}

constexpr int32 MaxNotifies = 32;
}

namespace AssetDocumentServerAssetFacts
{
TSharedRef<FJsonObject> DescribeBounds(const FBox& Box)
{
	TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
	const FBox Valid = Box.IsValid ? Box : FBox(FVector::ZeroVector, FVector::ZeroVector);
	Bounds->SetField(TEXT("Min"), Vector(Valid.Min));
	Bounds->SetField(TEXT("Max"), Vector(Valid.Max));
	Bounds->SetField(TEXT("Size"), Vector(Valid.GetSize()));
	Bounds->SetField(TEXT("Center"), Vector(Valid.GetCenter()));
	return Bounds;
}

TArray<TSharedPtr<FJsonValue>> DescribeMaterialSlots(const UObject& Mesh)
{
	TArray<TSharedPtr<FJsonValue>> Slots;
	auto Add = [&Slots](FName SlotName, const UMaterialInterface* Material)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("Index"), Slots.Num());
		Row->SetStringField(TEXT("SlotName"), SlotName.ToString());
		if (Material != nullptr)
		{
			Row->SetStringField(TEXT("Material"), Material->GetPathName());
		}
		else
		{
			Row->SetField(TEXT("Material"), MakeShared<FJsonValueNull>());
		}
		Slots.Add(MakeShared<FJsonValueObject>(Row));
	};
	if (const UStaticMesh* Static = Cast<UStaticMesh>(&Mesh))
	{
		for (const FStaticMaterial& Material : Static->GetStaticMaterials())
		{
			Add(Material.MaterialSlotName, Material.MaterialInterface);
		}
	}
	else if (const USkinnedAsset* Skinned = Cast<USkinnedAsset>(&Mesh))
	{
		for (const FSkeletalMaterial& Material : Skinned->GetMaterials())
		{
			Add(Material.MaterialSlotName, Material.MaterialInterface);
		}
	}
	return Slots;
}

TSharedPtr<FJsonObject> Describe(const UObject& Asset)
{
	TSharedPtr<FJsonObject> Facts = MakeShared<FJsonObject>();
	if (const UStaticMesh* Mesh = Cast<UStaticMesh>(&Asset))
	{
		Facts->SetStringField(TEXT("Kind"), TEXT("StaticMesh"));
		Facts->SetObjectField(TEXT("Bounds"), DescribeBounds(Mesh->GetBoundingBox()));
		TArray<FString> Sockets;
		for (const UStaticMeshSocket* Socket : Mesh->Sockets)
		{
			if (Socket != nullptr)
			{
				Sockets.Add(Socket->SocketName.ToString());
			}
		}
		if (!Sockets.IsEmpty())
		{
			Facts->SetArrayField(TEXT("Sockets"), Strings(Sockets));
		}
		Facts->SetArrayField(TEXT("MaterialSlots"), DescribeMaterialSlots(*Mesh));
		Facts->SetNumberField(TEXT("LODs"), Mesh->GetNumLODs());
		return Facts;
	}
	if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(&Asset))
	{
		Facts->SetStringField(TEXT("Kind"), TEXT("SkeletalMesh"));
		Facts->SetObjectField(TEXT("Bounds"), DescribeBounds(Skeletal->GetImportedBounds().GetBox()));
		Facts->SetArrayField(TEXT("MaterialSlots"), DescribeMaterialSlots(*Skeletal));
		return Facts;
	}
	if (const UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(&Asset))
	{
		Facts->SetStringField(TEXT("Kind"), TEXT("Animation"));
		Facts->SetNumberField(TEXT("PlayLength"), Round(Animation->GetPlayLength(), 0.0001));
		Facts->SetBoolField(TEXT("RootMotion"), Animation->HasRootMotion());
		Facts->SetNumberField(TEXT("NotifyCount"), Animation->Notifies.Num());
		TArray<TSharedPtr<FJsonValue>> Notifies;
		for (int32 Index = 0; Index < Animation->Notifies.Num() && Index < MaxNotifies; ++Index)
		{
			const FAnimNotifyEvent& Event = Animation->Notifies[Index];
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("Time"), Round(Event.GetTriggerTime(), 0.0001));
			FString Name = Event.NotifyName.ToString();
			if (Event.NotifyStateClass != nullptr)
			{
				Name = Event.NotifyStateClass->GetClass()->GetName();
				Row->SetNumberField(TEXT("Duration"), Round(Event.GetDuration(), 0.0001));
			}
			else if (Event.Notify != nullptr)
			{
				Name = Event.Notify->GetClass()->GetName();
			}
			Row->SetStringField(TEXT("Name"), Name);
			Row->SetNumberField(TEXT("Track"), Event.TrackIndex);
			Notifies.Add(MakeShared<FJsonValueObject>(Row));
		}
		Facts->SetArrayField(TEXT("Notifies"), Notifies);
		return Facts;
	}
	if (const USoundBase* Sound = Cast<USoundBase>(&Asset))
	{
		Facts->SetStringField(TEXT("Kind"), TEXT("Sound"));
		const float Duration = Sound->GetDuration();
		if (Duration >= INDEFINITELY_LOOPING_DURATION || Sound->IsLooping())
		{
			Facts->SetBoolField(TEXT("Looping"), true);
		}
		if (Duration < INDEFINITELY_LOOPING_DURATION)
		{
			Facts->SetNumberField(TEXT("Duration"), Round(Duration, 0.0001));
		}
		if (const USoundWave* Wave = Cast<USoundWave>(Sound))
		{
			Facts->SetNumberField(TEXT("Channels"), Wave->NumChannels);
			Facts->SetNumberField(TEXT("SampleRate"), Wave->GetSampleRateForCurrentPlatform());
		}
		return Facts;
	}
	return nullptr;
}
}
