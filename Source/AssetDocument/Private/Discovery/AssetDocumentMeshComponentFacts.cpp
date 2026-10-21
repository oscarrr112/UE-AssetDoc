// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Discovery/AssetDocumentDiscovery.h"

#include "AssetDocumentServerAssetFacts.h"

#include "Components/MeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

namespace UE::AssetDocument::Discovery::MeshComponentFacts
{
// Enough for any Blueprint an AI reads; the rest would only grow the inspect.
constexpr int32 MaxComponents = 32;

// The mesh a component renders: a static mesh or a skinned asset (Skeletal Mesh).
const UObject* ComponentMesh(const UMeshComponent& Component)
{
	if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(&Component))
	{
		return Static->GetStaticMesh();
	}
	if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(&Component))
	{
		return Skinned->GetSkinnedAsset();
	}
	return nullptr;
}

TSharedPtr<FJsonValue> PathOrNull(const UObject* Object)
{
	return Object != nullptr ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Object->GetPathName()))
		: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
}

TSharedPtr<FJsonValue> Row(const FString& Name, const TCHAR* Scope, const UMeshComponent& Component)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Name"), Name);
	Object->SetStringField(TEXT("Scope"), Scope);
	Object->SetStringField(TEXT("Class"), Component.GetClass()->GetPathName());
	const UObject* Mesh = ComponentMesh(Component);
	Object->SetField(TEXT("Mesh"), PathOrNull(Mesh));
	Object->SetArrayField(TEXT("MaterialSlots"), Mesh != nullptr
		? AssetDocumentServerAssetFacts::DescribeMaterialSlots(*Mesh) : TArray<TSharedPtr<FJsonValue>>());
	if (!Component.OverrideMaterials.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Overrides;
		for (const TObjectPtr<UMaterialInterface>& Material : Component.OverrideMaterials)
		{
			Overrides.Add(PathOrNull(Material.Get()));
		}
		Object->SetArrayField(TEXT("OverrideMaterials"), Overrides);
	}
	return MakeShared<FJsonValueObject>(Object);
}

// The name a Blueprint document gives a native component: its member property, else its subobject name.
FString NativeName(const UObject& Defaults, const UActorComponent& Component)
{
	for (TFieldIterator<FObjectPropertyBase> It(Defaults.GetClass()); It; ++It)
	{
		if (It->GetObjectPropertyValue_InContainer(&Defaults) == &Component)
		{
			return It->GetName();
		}
	}
	return Component.GetName();
}
}

namespace UE::AssetDocument::Discovery
{
TArray<TSharedPtr<FJsonValue>> DescribeMeshComponents(const UBlueprint& Blueprint)
{
	using namespace MeshComponentFacts;
	TArray<TSharedPtr<FJsonValue>> Rows;
	UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Blueprint.GeneratedClass);
	if (Generated == nullptr || !Generated->IsChildOf(AActor::StaticClass()))
	{
		return Rows;
	}
	// Native components: the class defaults' own subobjects hold this Blueprint's values.
	if (UObject* Defaults = Generated->GetDefaultObject(false))
	{
		TArray<UObject*> Subobjects;
		Defaults->GetDefaultSubobjects(Subobjects);
		for (const UObject* Subobject : Subobjects)
		{
			if (const UMeshComponent* Mesh = Cast<UMeshComponent>(Subobject); Mesh != nullptr && Rows.Num() < MaxComponents)
			{
				Rows.Add(Row(NativeName(*Defaults, *Mesh), TEXT("Native"), *Mesh));
			}
		}
	}
	// Components added in the Blueprint editor, parent Blueprints first; an inherited one with this Blueprint's
	// override.
	TArray<const UBlueprintGeneratedClass*> Hierarchy;
	UBlueprintGeneratedClass::GetGeneratedClassesHierarchy(Generated, Hierarchy);
	for (int32 Index = Hierarchy.Num() - 1; Index >= 0; --Index)
	{
		const USimpleConstructionScript* Script = Hierarchy[Index]->SimpleConstructionScript;
		if (Script == nullptr)
		{
			continue;
		}
		const TCHAR* Scope = Hierarchy[Index] == Generated ? TEXT("Owned") : TEXT("Inherited");
		for (const USCS_Node* Node : Script->GetAllNodes())
		{
			const UMeshComponent* Mesh = Node != nullptr ? Cast<UMeshComponent>(Node->GetActualComponentTemplate(Generated)) : nullptr;
			if (Mesh != nullptr && Rows.Num() < MaxComponents)
			{
				Rows.Add(Row(Node->GetVariableName().ToString(), Scope, *Mesh));
			}
		}
	}
	return Rows;
}
}
