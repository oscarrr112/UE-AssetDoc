// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"

#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "PropertyHandle.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback
{
namespace
{
bool ReadFields(IPropertyHandle& Handle, FPropertyReadContext& Context,
	const int32 Depth, FAssetDocumentRuntimeValueDto& OutValue,
	const TArray<FName>* ExactFields = nullptr)
{
	TMap<FString, TSharedRef<IPropertyHandle>> Fields;
	if (ExactFields)
	{
		for (const FName Name : *ExactFields)
		{
			const TSharedPtr<IPropertyHandle> Child = Handle.GetChildHandle(Name, false);
			if (!Child.IsValid() || !IsAuthoredProperty(*Child)) return false;
			Fields.Add(Name.ToString(), Child.ToSharedRef());
		}
	}
	else
	{
		uint32 Count = 0;
		if (Handle.GetNumChildren(Count) != FPropertyAccess::Success) return false;
		if (Count > 4096) { Context.bOverflow = true; return false; }
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			const TSharedPtr<IPropertyHandle> Child = Handle.GetChildHandle(Index);
			if (!Child.IsValid() || !Child->IsValidHandle() || !Child->GetProperty()) return false;
			if (!IsAuthoredProperty(*Child)) continue;
			const FString Key = Child->GetProperty()->GetName();
			if (Fields.Contains(Key)) return false;
			Fields.Add(Key, Child.ToSharedRef());
		}
	}
	TArray<FString> Keys;
	Fields.GetKeys(Keys);
	Keys.Sort();
	TArray<FAssetDocumentRuntimeValueDto> Values;
	for (const FString& Key : Keys)
	{
		FAssetDocumentRuntimeValueDto Value;
		if (!ReadAuthoredProperty(*Fields.FindChecked(Key), Context, Depth + 1, Value)) return false;
		Values.Add(MoveTemp(Value));
	}
	OutValue = ObjectValue(MoveTemp(Keys), MoveTemp(Values));
	return true;
}

bool ReadInstanced(IPropertyHandle& Handle, FPropertyReadContext& Context,
	const int32 Depth, FAssetDocumentRuntimeValueDto& OutValue)
{
	uint32 Count = 0;
	if (Handle.GetNumChildren(Count) != FPropertyAccess::Success) return false;
	if (Count > 4096) { Context.bOverflow = true; return false; }
	TSharedPtr<IPropertyHandle> StructureHandle;
	TSharedPtr<FStructOnScope> Instance;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr<IPropertyHandle> Child = Handle.GetChildHandle(Index);
		if (!Child.IsValid()) return false;
		// A customized vector/rotator wrapper gets a specialized handle rather
		// than IPropertyHandleStruct. GetOuterStructs still identifies the
		// structure node itself through the public provider-owned scope.
		const FStructProperty* WrapperProperty = CastField<FStructProperty>(Child->GetProperty());
		if (!Child->AsStruct().IsValid() && !WrapperProperty) continue;
		TArray<TSharedPtr<FStructOnScope>> Instances;
		Child->GetOuterStructs(Instances);
		if (Instances.Num() != 1 || StructureHandle.IsValid()) return false;
		if (WrapperProperty && Instances[0].IsValid()
			&& WrapperProperty->Struct != Instances[0]->GetStruct()) return false;
		StructureHandle = Child;
		Instance = Instances[0];
	}
	// Stock StructUtilsEditor customization adds a structure wrapper even for
	// a null instance. Absence means row generation was incomplete, not null.
	if (!StructureHandle.IsValid()) return false;
	if (!Instance.IsValid()) { OutValue = NullValue(); return true; }
	const UStruct* Type = Instance->GetStruct();
	if (!Type || !Cast<UScriptStruct>(Type)) return false;
	FAssetDocumentRuntimeValueDto Properties;
	if (!ReadFields(*StructureHandle, Context, Depth + 1, Properties)) return false;
	OutValue = ObjectValue({TEXT("Properties"), TEXT("Struct")},
		{MoveTemp(Properties), StringValue(Type->GetPathName())});
	return true;
}

bool ValueOrKeyFields(const UScriptStruct* Type, TArray<FName>& Fields)
{
	const bool bSimple = Type == FValueOrBBKey_Bool::StaticStruct()
		|| Type == FValueOrBBKey_Float::StaticStruct()
		|| Type == FValueOrBBKey_Int32::StaticStruct()
		|| Type == FValueOrBBKey_Name::StaticStruct()
		|| Type == FValueOrBBKey_String::StaticStruct()
		|| Type == FValueOrBBKey_Rotator::StaticStruct()
		|| Type == FValueOrBBKey_Vector::StaticStruct();
	Fields = {TEXT("DefaultValue"), TEXT("Key")};
	if (bSimple) return true;
	if (Type == FValueOrBBKey_Class::StaticStruct() || Type == FValueOrBBKey_Object::StaticStruct())
	{
		Fields.Add(TEXT("BaseClass"));
		return true;
	}
	if (Type == FValueOrBBKey_Enum::StaticStruct())
	{
		Fields.Add(TEXT("EnumType"));
		Fields.Add(TEXT("NativeEnumTypeName"));
		return true;
	}
	if (Type == FValueOrBBKey_Struct::StaticStruct())
	{
		Fields.Add(TEXT("bCanEditDefaultValueType"));
		return true;
	}
	return false;
}
}

bool ReadAuthoredStruct(IPropertyHandle& Handle, FPropertyReadContext& Context,
	const int32 Depth, FAssetDocumentRuntimeValueDto& OutValue)
{
	const FStructProperty* Property = CastField<FStructProperty>(Handle.GetProperty());
	if (!Property || !Property->Struct) return false;
	if (Property->Struct == FBlackboardKeySelector::StaticStruct())
	{
		const TSharedPtr<IPropertyHandle> Key = Handle.GetChildHandle(TEXT("SelectedKeyName"), false);
		FName Value;
		if (!Key.IsValid() || !IsAuthoredProperty(*Key)
			|| Key->GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = ObjectValue({TEXT("Key")}, {StringValue(Value.ToString())});
		return true;
	}
	if (Property->Struct == FInstancedStruct::StaticStruct())
		return ReadInstanced(Handle, Context, Depth, OutValue);
	TArray<FName> Fields;
	if (ValueOrKeyFields(Property->Struct, Fields))
		return ReadFields(Handle, Context, Depth, OutValue, &Fields);
	// Descendants of the special family cannot silently become generic structs.
	if (Property->Struct->IsChildOf(FValueOrBlackboardKeyBase::StaticStruct())) return false;
	return ReadFields(Handle, Context, Depth, OutValue);
}
}
