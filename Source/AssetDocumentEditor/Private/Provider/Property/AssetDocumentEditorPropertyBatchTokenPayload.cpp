// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"

#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

FAssetDocumentEditorPropertyBatchTokenPayload::
	FAssetDocumentEditorPropertyBatchTokenPayload(
		UObject& InTarget,
		TConstArrayView<FAssetDocumentEditorPropertyBatchTokenFieldView> InFields)
	: Target(&InTarget)
	, TargetClass(InTarget.GetClass())
	, TargetTypeRevision(static_cast<uint64>(InTarget.GetUniqueID()))
	, TargetClassRevision(
		GetAssetDocumentEditorPropertyTypeRevision(*InTarget.GetClass()))
{
	Fields.Reserve(InFields.Num());
	for (const FAssetDocumentEditorPropertyBatchTokenFieldView& Field : InFields)
	{
		FFieldBinding& Binding = Fields.AddDefaulted_GetRef();
		Binding.DeclaringClass = Field.DeclaringClass;
		Binding.Property = Field.Property;
		Binding.CodecId = Field.CodecId;
		Binding.TypeRevision = Field.TypeRevision;
		Binding.bReadable = Field.bReadable;
		Binding.bWritable = Field.bWritable;
		Binding.bDeclared = Field.bDeclared;
	}
}

FName FAssetDocumentEditorPropertyBatchTokenPayload::GetPayloadTypeId() const
{
	return TEXT("assetdocument.editor.property_batch_token.v1");
}

bool FAssetDocumentEditorPropertyBatchTokenPayload::IsAlive() const
{
	FAssetDocumentEditorPropertyBatchTokenView View;
	return TryBuildView(View);
}

bool FAssetDocumentEditorPropertyBatchTokenPayload::TryGetView(
	FAssetDocumentEditorPropertyBatchTokenView& OutView) const
{
	OutView = {};
	FAssetDocumentEditorPropertyBatchTokenView View;
	if (!TryBuildView(View))
	{
		return false;
	}
	OutView = MoveTemp(View);
	return true;
}

bool FAssetDocumentEditorPropertyBatchTokenPayload::TryResolveTarget(
	UObject*& OutTarget,
	UClass*& OutTargetClass) const
{
	OutTarget = Target.Get();
	OutTargetClass = TargetClass.Get();
	if (OutTarget == nullptr || OutTargetClass == nullptr)
	{
		return false;
	}
	if (OutTarget->GetClass() != OutTargetClass)
	{
		return false;
	}
	return static_cast<uint64>(OutTarget->GetUniqueID())
		== TargetTypeRevision;
}

bool FAssetDocumentEditorPropertyBatchTokenPayload::TryResolveField(
	const FFieldBinding& Binding,
	UClass*& OutDeclaringClass,
	FProperty*& OutProperty)
{
	OutDeclaringClass = Binding.DeclaringClass.Get();
	OutProperty = Binding.Property.Get();
	if (OutDeclaringClass == nullptr || OutProperty == nullptr)
	{
		return false;
	}
	return OutProperty->GetOwnerClass() == OutDeclaringClass;
}

bool FAssetDocumentEditorPropertyBatchTokenPayload::IsFieldContractLive(
	const FFieldBinding& Binding,
	const UClass& DeclaringClass)
{
	return !Binding.CodecId.IsEmpty()
		&& Binding.bReadable
		&& GetAssetDocumentEditorPropertyTypeRevision(DeclaringClass)
			== Binding.TypeRevision;
}

bool FAssetDocumentEditorPropertyBatchTokenPayload::TryAppendFieldView(
	const FFieldBinding& Binding,
	TArray<FAssetDocumentEditorPropertyBatchTokenFieldView>& OutFields)
{
	UClass* DeclaringClass = nullptr;
	FProperty* Property = nullptr;
	if (!TryResolveField(Binding, DeclaringClass, Property))
	{
		return false;
	}
	if (!IsFieldContractLive(Binding, *DeclaringClass))
	{
		return false;
	}
	FAssetDocumentEditorPropertyBatchTokenFieldView& Field =
		OutFields.AddDefaulted_GetRef();
	Field.DeclaringClass = DeclaringClass;
	Field.Property = Property;
	Field.CodecId = Binding.CodecId;
	Field.TypeRevision = Binding.TypeRevision;
	Field.bReadable = Binding.bReadable;
	Field.bWritable = Binding.bWritable;
	Field.bDeclared = Binding.bDeclared;
	return true;
}

bool FAssetDocumentEditorPropertyBatchTokenPayload::TryBuildView(
	FAssetDocumentEditorPropertyBatchTokenView& OutView) const
{
	UObject* ResolvedTarget = nullptr;
	UClass* ResolvedTargetClass = nullptr;
	if (!TryResolveTarget(ResolvedTarget, ResolvedTargetClass))
	{
		return false;
	}
	if (GetAssetDocumentEditorPropertyTypeRevision(*ResolvedTargetClass)
		!= TargetClassRevision)
	{
		return false;
	}
	OutView.Target = ResolvedTarget;
	OutView.TargetClass = ResolvedTargetClass;
	OutView.TargetTypeRevision = TargetTypeRevision;
	OutView.Fields.Reserve(Fields.Num());
	for (const FFieldBinding& Binding : Fields)
	{
		if (!TryAppendFieldView(Binding, OutView.Fields))
		{
			return false;
		}
	}
	return true;
}
