// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class IInterface_AssetUserData;
class UClass;
class UObject;

struct FAssetDocumentEditorEmbeddedAdmission
{
	FString CategoryId;
	UObject* Outer = nullptr;
	UClass* ObjectClass = nullptr;
	IInterface_AssetUserData* Owner = nullptr;
};

bool TryAdmitAssetDocumentEditorEmbeddedObject(
	const FAssetDocumentRuntimeValueDto& Arguments,
	UObject& Outer,
	UClass& ObjectClass,
	FAssetDocumentEditorEmbeddedAdmission& OutAdmission);
