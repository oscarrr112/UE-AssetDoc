// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/FieldPath.h"

class FProperty;
class UClass;
class UObject;

struct FAssetDocumentEditorPropertyBatchTokenFieldView
{
	UClass* DeclaringClass = nullptr;
	FProperty* Property = nullptr;
	FString CodecId;
	uint64 TypeRevision = 0;
	bool bReadable = false;
	bool bWritable = false;
	bool bDeclared = false;
};

struct FAssetDocumentEditorPropertyBatchTokenView
{
	UObject* Target = nullptr;
	UClass* TargetClass = nullptr;
	uint64 TargetTypeRevision = 0;
	TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields;
};

class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyBatchTokenPayload final
	: public IAssetDocumentNativeHandlePayload
{
public:
	FAssetDocumentEditorPropertyBatchTokenPayload(
		UObject& InTarget,
		TConstArrayView<FAssetDocumentEditorPropertyBatchTokenFieldView> InFields);

	virtual FName GetPayloadTypeId() const override;
	virtual bool IsAlive() const override;
	bool TryGetView(FAssetDocumentEditorPropertyBatchTokenView& OutView) const;

private:
	struct FFieldBinding
	{
		TWeakObjectPtr<UClass> DeclaringClass;
		TFieldPath<FProperty> Property;
		FString CodecId;
		uint64 TypeRevision = 0;
		bool bReadable = false;
		bool bWritable = false;
		bool bDeclared = false;
	};

	bool TryResolveTarget(
		UObject*& OutTarget,
		UClass*& OutTargetClass) const;
	static bool TryResolveField(
		const FFieldBinding& Binding,
		UClass*& OutDeclaringClass,
		FProperty*& OutProperty);
	static bool IsFieldContractLive(
		const FFieldBinding& Binding,
		const UClass& DeclaringClass);
	static bool TryAppendFieldView(
		const FFieldBinding& Binding,
		TArray<FAssetDocumentEditorPropertyBatchTokenFieldView>& OutFields);
	bool TryBuildView(
		FAssetDocumentEditorPropertyBatchTokenView& OutView) const;

	TWeakObjectPtr<UObject> Target;
	TWeakObjectPtr<UClass> TargetClass;
	uint64 TargetTypeRevision = 0;
	uint64 TargetClassRevision = 0;
	TArray<FFieldBinding> Fields;
};
