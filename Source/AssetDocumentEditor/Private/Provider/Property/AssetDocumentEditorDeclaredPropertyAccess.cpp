// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Property/AssetDocumentEditorDeclaredPropertyAccess.h"

#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

bool IsAssetDocumentEditorDeclarableProperty(const FProperty& Property)
{
	constexpr uint64 RejectedFlags = CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated
		| CPF_SkipSerialization | CPF_EditConst;
	return Property.GetOwnerClass() != nullptr && !Property.HasAnyPropertyFlags(RejectedFlags);
}

bool GetAssetDocumentEditorDeclaredPropertyValue(
	const UObject& Object,
	const FProperty& Property,
	void* OutValue)
{
	if (OutValue == nullptr || !IsAssetDocumentEditorDeclarableProperty(Property)
		|| !Object.IsA(Property.GetOwnerClass()))
	{
		return false;
	}
	return PropertyAccessUtil::CopyCompletePropertyValue(
		&Property, Property.ContainerPtrToValuePtr<void>(&Object), &Property, OutValue);
}

bool SetAssetDocumentEditorDeclaredPropertyValue(
	UObject& Object,
	const FProperty& Property,
	const void* Value,
	EPropertyAccessChangeNotifyMode NotifyMode)
{
	// CDOs and archetypes would need archetype-instance propagation; declared
	// surfaces write asset instances only. (PropertyAccessUtil::IsObjectTemplate
	// also reports every asset as a template, so it is not the check here.)
	if (Value == nullptr || !IsAssetDocumentEditorDeclarableProperty(Property)
		|| !Object.IsA(Property.GetOwnerClass()) || Object.IsTemplate())
	{
		return false;
	}
	void* Destination = Property.ContainerPtrToValuePtr<void>(&Object);
	return PropertyAccessUtil::SetPropertyValue(
		[&Property, Value, Destination](const FPropertyAccessChangeNotify* ChangeNotify)
		{
			const bool bIdentical = PropertyAccessUtil::IsCompletePropertyIdentical(
				&Property, Value, &Property, Destination);
			PropertyAccessUtil::EmitPreChangeNotify(ChangeNotify, bIdentical);
			const bool bCopied = bIdentical
				|| PropertyAccessUtil::CopyCompletePropertyValue(&Property, Value, &Property, Destination);
			PropertyAccessUtil::EmitPostChangeNotify(ChangeNotify, bIdentical);
			return bCopied;
		},
		[&Property, &Object, NotifyMode]()
		{
			return PropertyAccessUtil::BuildBasicChangeNotify(
				&Property, &Object, NotifyMode, EPropertyChangeType::ValueSet);
		}) == EPropertyAccessResultFlags::Success;
}
