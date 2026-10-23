// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "Widgets/SWindow.h"

#include "AssetDocumentEditorFactoryFailureTestTypes.generated.h"

// Test-only asset class whose only factory fails (FactoryCreateNew returns
// null after IAssetTools::CreateAsset already created the target package).
UCLASS(NotBlueprintable)
class UAssetDocumentEditorFactoryFailureTestAsset : public UObject
{
	GENERATED_BODY()
};

UCLASS(Transient, HideCategories = Object)
class UAssetDocumentEditorFailingTestFactory : public UFactory
{
	GENERATED_BODY()

public:
	UAssetDocumentEditorFailingTestFactory()
	{
		SupportedClass = UAssetDocumentEditorFactoryFailureTestAsset::StaticClass();
		bCreateNew = true;
		bEditAfterNew = false;
	}

	virtual bool ShouldShowInNewMenu() const override { return false; }

	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override
	{
		return nullptr;
	}
};

// Test-only asset class whose only factory opens a Slate modal window inside
// FactoryCreateNew (the GroomFactory pattern) and creates a side package first.
// The MCP create path must refuse it without blocking and without residue.
UCLASS(NotBlueprintable)
class UAssetDocumentEditorInteractiveTestAsset : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = Test)
	float TestFloat = 0.5f;
};

UCLASS(Transient, HideCategories = Object)
class UAssetDocumentEditorModalTestFactory : public UFactory
{
	GENERATED_BODY()

public:
	UAssetDocumentEditorModalTestFactory()
	{
		SupportedClass = UAssetDocumentEditorInteractiveTestAsset::StaticClass();
		bCreateNew = true;
		bEditAfterNew = false;
	}

	virtual bool ShouldShowInNewMenu() const override { return false; }

	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override
	{
		SidePackageName = FString::Printf(TEXT("/Temp/AssetDocumentModalSide_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		CreatePackage(*SidePackageName);
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().AddModalWindow(
				SNew(SWindow).Title(FText::FromString(TEXT("AssetDocument modal test"))).ClientSize(FVector2D(320, 120)),
				nullptr);
		}
		return NewObject<UAssetDocumentEditorInteractiveTestAsset>(InParent, InClass, InName, Flags);
	}

	// Last side package name, for the residue check (shared by all instances).
	static inline FString SidePackageName;
};
