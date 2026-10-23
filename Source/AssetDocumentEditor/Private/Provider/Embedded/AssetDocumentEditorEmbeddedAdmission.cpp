// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Embedded/AssetDocumentEditorEmbeddedAdmission.h"

#include "Engine/AssetUserData.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "UObject/Class.h"

namespace
{
bool TryReadAssetUserDataArguments(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FString& OutCategoryId)
{
	const TArray<FAssetDocumentEditorValueFieldSpec> Fields = {
		{TEXT("category_id"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("properties"), EAssetDocumentRuntimeValueKind::Object},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Arguments, Fields, Values, DiagnosticCode))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& Properties = *Values[1];
	if (!Properties.ObjectKeys.IsEmpty()
		|| !Properties.Elements.IsEmpty())
	{
		return false;
	}
	OutCategoryId = Values[0]->StringValue;
	return OutCategoryId == TEXT("asset_user_data.v1");
}

bool IsAssetUserDataClassAdmitted(const UClass& ObjectClass)
{
	return ObjectClass.IsChildOf(UAssetUserData::StaticClass())
		&& !ObjectClass.HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
}
}

bool TryAdmitAssetDocumentEditorEmbeddedObject(
	const FAssetDocumentRuntimeValueDto& Arguments,
	UObject& Outer,
	UClass& ObjectClass,
	FAssetDocumentEditorEmbeddedAdmission& OutAdmission)
{
	FString CategoryId;
	if (!TryReadAssetUserDataArguments(Arguments, CategoryId)
		|| !IsAssetUserDataClassAdmitted(ObjectClass))
	{
		return false;
	}
	IInterface_AssetUserData* Owner =
		Cast<IInterface_AssetUserData>(&Outer);
	if (Owner == nullptr
		|| Owner->GetAssetUserDataOfClass(&ObjectClass) != nullptr)
	{
		return false;
	}
	OutAdmission.CategoryId = MoveTemp(CategoryId);
	OutAdmission.Outer = &Outer;
	OutAdmission.ObjectClass = &ObjectClass;
	OutAdmission.Owner = Owner;
	return true;
}
