// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetDocumentServerAssetFacts.h"
#include "Dom/JsonValue.h"
#include "EditorReimportHandler.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Import/AssetImportTool.h"
#include "Materials/MaterialInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Sound/SoundWave.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
double Round3(double Value)
{
	return FMath::RoundToDouble(Value * 1000.0) / 1000.0;
}

FString PathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

template<typename EnumType>
FString EnumName(EnumType Value)
{
	return StaticEnum<EnumType>()->GetNameStringByValue(static_cast<int64>(Value));
}

TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const double Value : Values)
	{
		Out.Add(MakeShared<FJsonValueNumber>(Value));
	}
	return Out;
}

void StaticMeshFacts(UStaticMesh& Mesh, FJsonObject& Json)
{
	if (Mesh.GetNumLODs() > 0)
	{
		Json.SetNumberField(TEXT("Vertices"), Mesh.GetNumVertices(0));
		Json.SetNumberField(TEXT("Triangles"), Mesh.GetNumTriangles(0));
	}
	const UBodySetup* Body = Mesh.GetBodySetup();
	Json.SetNumberField(TEXT("CollisionShapes"), Body != nullptr ? Body->AggGeom.GetElementCount() : 0);
	Json.SetBoolField(TEXT("Nanite"), Mesh.IsNaniteEnabled());
}

void SkeletalMeshFacts(USkeletalMesh& Mesh, FJsonObject& Json)
{
	Json.SetNumberField(TEXT("Bones"), Mesh.GetRefSkeleton().GetNum());
	Json.SetStringField(TEXT("Skeleton"), PathOf(Mesh.GetSkeleton()));
	Json.SetNumberField(TEXT("LODs"), Mesh.GetLODNum());
	if (Mesh.GetPhysicsAsset() != nullptr)
	{
		Json.SetStringField(TEXT("PhysicsAsset"), PathOf(Mesh.GetPhysicsAsset()));
	}
}

void AnimSequenceFacts(UAnimSequence& Sequence, FJsonObject& Json)
{
	Json.SetNumberField(TEXT("Frames"), Sequence.GetNumberOfSampledKeys());
	Json.SetNumberField(TEXT("FrameRate"), Round3(Sequence.GetSamplingFrameRate().AsDecimal()));
	Json.SetStringField(TEXT("Skeleton"), PathOf(Sequence.GetSkeleton()));
	if (const IAnimationDataModel* Model = Sequence.GetDataModelInterface().GetInterface())
	{
		Json.SetNumberField(TEXT("BoneTracks"), Model->GetNumBoneTracks());
		Json.SetNumberField(TEXT("Curves"), Model->GetNumberOfFloatCurves());
	}
}

void TextureFacts(UTexture& Texture, FJsonObject& Json)
{
	Json.SetStringField(TEXT("Kind"), TEXT("Texture"));
	Json.SetArrayField(TEXT("Size"), Numbers({static_cast<double>(Texture.Source.GetSizeX()), static_cast<double>(Texture.Source.GetSizeY())}));
	Json.SetBoolField(TEXT("SRGB"), Texture.SRGB);
	Json.SetStringField(TEXT("Compression"), EnumName(Texture.CompressionSettings.GetValue()));
	Json.SetStringField(TEXT("LODGroup"), EnumName(Texture.LODGroup.GetValue()));
	Json.SetNumberField(TEXT("SourceMips"), Texture.Source.GetNumMips());
	Json.SetBoolField(TEXT("HDR"), Texture.HasHDRSource());
}
} // namespace

TSharedRef<FJsonObject> MakeImportedAssetFacts(UObject& Asset)
{
	// The facts inspect reports (AssetDocumentServerAssetFacts: bounds, animation length, sound
	// duration), plus what an import result needs to confirm the options took effect.
	TSharedPtr<FJsonObject> Described = AssetDocumentServerAssetFacts::Describe(Asset);
	TSharedRef<FJsonObject> Json = Described.IsValid() ? Described.ToSharedRef() : MakeShared<FJsonObject>();
	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(&Asset))
	{
		StaticMeshFacts(*StaticMesh, *Json);
	}
	else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(&Asset))
	{
		SkeletalMeshFacts(*SkeletalMesh, *Json);
	}
	else if (USkeleton* Skeleton = Cast<USkeleton>(&Asset))
	{
		Json->SetStringField(TEXT("Kind"), TEXT("Skeleton"));
		Json->SetNumberField(TEXT("Bones"), Skeleton->GetReferenceSkeleton().GetNum());
	}
	else if (UAnimSequence* Sequence = Cast<UAnimSequence>(&Asset))
	{
		AnimSequenceFacts(*Sequence, *Json);
	}
	else if (UTexture* Texture = Cast<UTexture>(&Asset))
	{
		TextureFacts(*Texture, *Json);
	}
	else if (USoundWave* Sound = Cast<USoundWave>(&Asset))
	{
		Json->SetNumberField(TEXT("ImportedSampleRate"), Sound->GetImportedSampleRate());
	}
	else if (UPhysicsAsset* Physics = Cast<UPhysicsAsset>(&Asset))
	{
		Json->SetStringField(TEXT("Kind"), TEXT("PhysicsAsset"));
		Json->SetNumberField(TEXT("Bodies"), Physics->SkeletalBodySetups.Num());
	}
	else if (UMaterialInstance* Instance = Cast<UMaterialInstance>(&Asset))
	{
		Json->SetStringField(TEXT("Kind"), TEXT("MaterialInstance"));
		Json->SetStringField(TEXT("Parent"), PathOf(Instance->Parent));
	}
	TArray<FString> Sources;
	if (FReimportManager::Instance()->CanReimport(&Asset, &Sources) && !Sources.IsEmpty() && !Sources[0].IsEmpty())
	{
		Json->SetStringField(TEXT("Source"), Sources[0]);
	}
	return Json;
}
}
