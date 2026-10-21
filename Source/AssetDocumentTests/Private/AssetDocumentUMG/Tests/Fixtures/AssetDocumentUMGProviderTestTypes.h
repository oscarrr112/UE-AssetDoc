// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

#include "AssetDocumentUMGProviderTestTypes.generated.h"

UCLASS()
class UAssetDocumentUMGProviderTestUserWidget final : public UUserWidget
{
	GENERATED_BODY()
};

class FAssetDocumentUMGProviderTestPayload final
	: public IAssetDocumentNativeHandlePayload
{
public:
	explicit FAssetDocumentUMGProviderTestPayload(FName InTypeId);

	virtual FName GetPayloadTypeId() const override;
	virtual bool IsAlive() const override;

	void Invalidate();

private:
	FName TypeId;
	bool bAlive = true;
};
