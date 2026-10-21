// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class UWidgetBlueprint;
class UWidget;
class UWidgetAnimation;
class UUserWidget;

class FAssetDocumentUMGProviderTestFixture final
{
public:
	~FAssetDocumentUMGProviderTestFixture();

	bool CreateTransientWidgetBlueprint();
	void DestroyTransientWidgetBlueprint();
	UWidgetBlueprint* GetWidgetBlueprint() const;
	UWidget* AddFixtureWidget(UClass* WidgetClass, FName Name);
	UWidgetAnimation* AddFixtureAnimation(FName Name);
	UUserWidget* CreatePreviewRoot(FName Name);
	UWidget* CreatePreviewWidget(
		UClass* WidgetClass,
		FName Name,
		UUserWidget& PreviewRoot);
	FString CaptureDigest() const;

private:
	TObjectPtr<UWidgetBlueprint> WidgetBlueprint = nullptr;
	TArray<TObjectPtr<UObject>> StrongTransientObjects;
	FString FixtureName;
};
