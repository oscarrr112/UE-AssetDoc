// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AnimAssets/AssetDocumentBlendSpaceEdits.h"

#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "AssetDocumentGraphAdapters.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Animation::BlendSpaceEdits
{
namespace
{
const FName BlendParametersName(TEXT("BlendParameters"));
const FName InterpolationParamName(TEXT("InterpolationParam"));
const FName SampleDataName(TEXT("SampleData"));

bool IsEditableField(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated);
}
} // namespace

int32 NumAxes(const UBlendSpace& BlendSpace)
{
	// UAimOffsetBlendSpace1D derives from UBlendSpace1D.
	return BlendSpace.IsA<UBlendSpace1D>() ? 1 : 2;
}

FStructProperty* AxisProperty()
{
	return FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), BlendParametersName);
}

FStructProperty* InterpolationProperty()
{
	return FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), InterpolationParamName);
}

FProperty* SampleDataProperty()
{
	return FindFProperty<FArrayProperty>(UBlendSpace::StaticClass(), SampleDataName);
}

FBlendParameter* GetAxis(UBlendSpace& BlendSpace, int32 Axis)
{
	FStructProperty* Property = AxisProperty();
	if (Property == nullptr || Property->Struct != FBlendParameter::StaticStruct() || Axis < 0 || Axis >= Property->ArrayDim)
	{
		return nullptr;
	}
	return Property->ContainerPtrToValuePtr<FBlendParameter>(&BlendSpace, Axis);
}

FBlendSample* GetSample(UBlendSpace& BlendSpace, int32 Index)
{
	FArrayProperty* Property = CastField<FArrayProperty>(SampleDataProperty());
	const FStructProperty* Inner = Property != nullptr ? CastField<FStructProperty>(Property->Inner) : nullptr;
	if (Inner == nullptr || Inner->Struct != FBlendSample::StaticStruct())
	{
		return nullptr;
	}
	TArray<FBlendSample>& Samples = *Property->ContainerPtrToValuePtr<TArray<FBlendSample>>(&BlendSpace);
	return Samples.IsValidIndex(Index) ? &Samples[Index] : nullptr;
}

void NotifyPostChange(UBlendSpace& BlendSpace)
{
	BlendSpace.ValidateSampleData();
	BlendSpace.ResampleData();
	BlendSpace.MarkPackageDirty();
}

bool WriteDetailsValue(UBlendSpace& BlendSpace, FProperty& Top, FProperty& Leaf, void* LeafPtr, const FValue& Value, FString& OutError)
{
	BlendSpace.Modify();
	BlendSpace.PreEditChange(&Leaf);
	const bool bDecoded = DecodeAssetDocumentGraphPropertyValue(Leaf, LeafPtr, Value, OutError);
	FPropertyChangedEvent Event(&Leaf, EPropertyChangeType::ValueSet);
	Event.MemberProperty = &Top;
	BlendSpace.PostEditChangeProperty(Event);
	NotifyPostChange(BlendSpace);
	return bDecoded;
}

bool WriteAxisField(UBlendSpace& BlendSpace, int32 Axis, FName Field, const FValue& Value, FString& OutError)
{
	FStructProperty* Top = AxisProperty();
	FBlendParameter* Parameter = GetAxis(BlendSpace, Axis);
	FProperty* Leaf = Top != nullptr ? Top->Struct->FindPropertyByName(Field) : nullptr;
	if (Parameter == nullptr || Leaf == nullptr || !IsEditableField(*Leaf))
	{
		OutError = FString::Printf(TEXT("'%s' is not an axis setting."), *Field.ToString());
		return false;
	}
	return WriteDetailsValue(BlendSpace, *Top, *Leaf, Leaf->ContainerPtrToValuePtr<void>(Parameter), Value, OutError);
}

bool WriteSampleField(UBlendSpace& BlendSpace, int32 Index, FName Field, const FValue& Value, FString& OutError)
{
	FProperty* Top = SampleDataProperty();
	FBlendSample* Sample = GetSample(BlendSpace, Index);
	FProperty* Leaf = FBlendSample::StaticStruct()->FindPropertyByName(Field);
	if (Top == nullptr || Sample == nullptr || Leaf == nullptr || !IsEditableField(*Leaf))
	{
		OutError = FString::Printf(TEXT("'%s' is not a sample setting."), *Field.ToString());
		return false;
	}
	return WriteDetailsValue(BlendSpace, *Top, *Leaf, Leaf->ContainerPtrToValuePtr<void>(Sample), Value, OutError);
}
} // namespace UE::AssetDocument::Animation::BlendSpaceEdits
