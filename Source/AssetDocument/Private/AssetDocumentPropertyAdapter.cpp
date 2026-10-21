// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentPropertyAdapter.h"

#include "Utils/PropertySetterUtils.h"

#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/FieldIterator.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"

namespace
{
bool IsTypeName(const FString& TypeName, std::initializer_list<const TCHAR*> AcceptedNames)
{
	for (const TCHAR* AcceptedName : AcceptedNames)
	{
		if (TypeName.Equals(AcceptedName, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

FString JoinSelectorPolicyPath(const FString& Base, const FString& Segment)
{
	return Base.IsEmpty() ? Segment : FString::Printf(TEXT("%s/%s"), *Base, *Segment);
}

void CollectSelectorPoliciesFromValue(
	FProperty* Property,
	const void* ValuePtr,
	const FString& Path,
	TArray<TSharedPtr<FJsonValue>>& OutPolicies);

void CollectSelectorPoliciesFromDefaultValue(
	FProperty* Property,
	const FString& Path,
	TArray<TSharedPtr<FJsonValue>>& OutPolicies)
{
	const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
	if (!StructProperty || !StructProperty->Struct)
	{
		return;
	}
	FStructOnScope DefaultValue(StructProperty->Struct);
	CollectSelectorPoliciesFromValue(Property, DefaultValue.GetStructMemory(), Path, OutPolicies);
}

void AddSelectorPolicy(
	const FBlackboardKeySelector& Selector,
	const FString& Path,
	TArray<TSharedPtr<FJsonValue>>& OutPolicies)
{
	TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
	Policy->SetStringField(TEXT("path"), Path);
	Policy->SetStringField(TEXT("authored_field"), TEXT("Key"));
	Policy->SetBoolField(TEXT("read_only"), true);
	const FBoolProperty* NoneAllowedProperty = FindFProperty<FBoolProperty>(
		FBlackboardKeySelector::StaticStruct(),
		TEXT("bNoneIsAllowedValue"));
	Policy->SetBoolField(
		TEXT("none_allowed"),
		NoneAllowedProperty && NoneAllowedProperty->GetPropertyValue_InContainer(&Selector));

	TArray<TSharedPtr<FJsonValue>> AllowedTypes;
	for (const UBlackboardKeyType* Filter : Selector.AllowedTypes)
	{
		if (!Filter)
		{
			continue;
		}
		TSharedRef<FJsonObject> FilterPolicy = MakeShared<FJsonObject>();
		FilterPolicy->SetStringField(TEXT("filter_type"), Filter->GetClass()->GetPathName());
		if (const UBlackboardKeyType_Object* ObjectFilter = Cast<UBlackboardKeyType_Object>(Filter))
		{
			FilterPolicy->SetStringField(
				TEXT("base_class"),
				ObjectFilter->BaseClass ? ObjectFilter->BaseClass->GetPathName() : FString());
		}
		else if (const UBlackboardKeyType_Class* ClassFilter = Cast<UBlackboardKeyType_Class>(Filter))
		{
			FilterPolicy->SetStringField(
				TEXT("base_class"),
				ClassFilter->BaseClass ? ClassFilter->BaseClass->GetPathName() : FString());
		}
		AllowedTypes.Add(MakeShared<FJsonValueObject>(FilterPolicy));
	}
	Policy->SetArrayField(TEXT("allowed_types"), AllowedTypes);
	OutPolicies.Add(MakeShared<FJsonValueObject>(Policy));
}

void CollectSelectorPoliciesFromStruct(
	UScriptStruct* Struct,
	const void* StructValue,
	const FString& Path,
	TArray<TSharedPtr<FJsonValue>>& OutPolicies)
{
	if (!Struct || !StructValue)
	{
		return;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		CollectSelectorPoliciesFromValue(
			Property,
			Property->ContainerPtrToValuePtr<void>(StructValue),
			JoinSelectorPolicyPath(Path, Property->GetName()),
			OutPolicies);
	}
}

void CollectSelectorPoliciesFromValue(
	FProperty* Property,
	const void* ValuePtr,
	const FString& Path,
	TArray<TSharedPtr<FJsonValue>>& OutPolicies)
{
	if (!Property || !ValuePtr)
	{
		return;
	}
	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (StructProperty->Struct == FBlackboardKeySelector::StaticStruct())
		{
			AddSelectorPolicy(*static_cast<const FBlackboardKeySelector*>(ValuePtr), Path, OutPolicies);
			return;
		}
		if (StructProperty->Struct == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct* Instanced = static_cast<const FInstancedStruct*>(ValuePtr);
			if (Instanced->IsValid())
			{
				CollectSelectorPoliciesFromStruct(
					const_cast<UScriptStruct*>(Instanced->GetScriptStruct()),
					Instanced->GetMemory(),
					JoinSelectorPolicyPath(Path, TEXT("Properties")),
					OutPolicies);
			}
			return;
		}
		CollectSelectorPoliciesFromStruct(StructProperty->Struct, ValuePtr, Path, OutPolicies);
		return;
	}
	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		const FString ElementPath = JoinSelectorPolicyPath(Path, TEXT("*"));
		if (Helper.Num() > 0)
		{
			CollectSelectorPoliciesFromValue(
				ArrayProperty->Inner,
				Helper.GetRawPtr(0),
				ElementPath,
				OutPolicies);
		}
		else
		{
			CollectSelectorPoliciesFromDefaultValue(ArrayProperty->Inner, ElementPath, OutPolicies);
		}
	}
	else if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		FScriptMapHelper Helper(MapProperty, ValuePtr);
		const FString ValuePath = JoinSelectorPolicyPath(Path, TEXT("*"));
		bool bCollectedRepresentative = false;
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (Helper.IsValidIndex(Index))
			{
				CollectSelectorPoliciesFromValue(
					MapProperty->ValueProp,
					Helper.GetValuePtr(Index),
					ValuePath,
					OutPolicies);
				bCollectedRepresentative = true;
				break;
			}
		}
		if (!bCollectedRepresentative)
		{
			CollectSelectorPoliciesFromDefaultValue(MapProperty->ValueProp, ValuePath, OutPolicies);
		}
	}
	else if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper Helper(SetProperty, ValuePtr);
		const FString ElementPath = JoinSelectorPolicyPath(Path, TEXT("*"));
		bool bCollectedRepresentative = false;
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (Helper.IsValidIndex(Index))
			{
				CollectSelectorPoliciesFromValue(
					SetProperty->ElementProp,
					Helper.GetElementPtr(Index),
					ElementPath,
					OutPolicies);
				bCollectedRepresentative = true;
				break;
			}
		}
		if (!bCollectedRepresentative)
		{
			CollectSelectorPoliciesFromDefaultValue(SetProperty->ElementProp, ElementPath, OutPolicies);
		}
	}
}
}

FString FAssetDocumentPropertyAdapter::GetTypeToken(FProperty* Property)
{
	if (!Property)
	{
		return TEXT("Unknown");
	}

	if (CastField<FBoolProperty>(Property))
	{
		return TEXT("Bool");
	}

	if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		return EnumProperty->GetEnum() ? TEXT("Enum") : TEXT("Int");
	}

	if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		return ByteProperty->Enum ? TEXT("Enum") : TEXT("Int");
	}

	if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
	{
		return NumericProperty->IsFloatingPoint() ? TEXT("Float") : TEXT("Int");
	}

	if (CastField<FStrProperty>(Property))
	{
		return TEXT("String");
	}

	if (CastField<FNameProperty>(Property))
	{
		return TEXT("Name");
	}

	if (CastField<FTextProperty>(Property))
	{
		return TEXT("Text");
	}

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (StructProperty->Struct)
		{
			return StructProperty->Struct->GetStructCPPName();
		}
		return TEXT("Struct");
	}

	if (CastField<FSoftClassProperty>(Property) || CastField<FClassProperty>(Property))
	{
		return TEXT("Class");
	}

	if (CastField<FSoftObjectProperty>(Property) || CastField<FObjectPropertyBase>(Property))
	{
		return TEXT("Object");
	}

	if (CastField<FArrayProperty>(Property))
	{
		return TEXT("Array");
	}

	if (CastField<FMapProperty>(Property))
	{
		return TEXT("Map");
	}

	if (CastField<FSetProperty>(Property))
	{
		return TEXT("Set");
	}

	return TEXT("Unsupported");
}

bool FAssetDocumentPropertyAdapter::IsWritableProperty(FProperty* Property)
{
	return Property
		&& Property->HasAnyPropertyFlags(CPF_Edit)
		&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst);
}

FString FAssetDocumentPropertyAdapter::GetNonWritableReason(FProperty* Property)
{
	if (!Property)
	{
		return TEXT("missing-property");
	}

	if (Property->HasAnyPropertyFlags(CPF_Transient))
	{
		return TEXT("transient");
	}

	if (Property->HasAnyPropertyFlags(CPF_Deprecated))
	{
		return TEXT("deprecated");
	}

	if (Property->HasAnyPropertyFlags(CPF_EditConst))
	{
		return TEXT("edit-const");
	}

	if (!Property->HasAnyPropertyFlags(CPF_Edit))
	{
		return TEXT("non-editable");
	}

	return FString();
}

TSharedPtr<FJsonValue> FAssetDocumentPropertyAdapter::ExtractPropertyValue(FProperty* Property, const void* ValuePtr)
{
	return FPropertySetterUtils::ExtractPropertyToJson(Property, ValuePtr);
}

TSharedPtr<FJsonObject> FAssetDocumentPropertyAdapter::ExtractWritablePropertiesToJson(
	UObject* Object,
	bool bSkipDefaults,
	const TSet<FName>* ExcludedPropertyNames)
{
	if (!Object)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
	UClass* ObjectClass = Object->GetClass();
	UObject* DefaultObject = bSkipDefaults ? ObjectClass->GetDefaultObject() : nullptr;

	for (TFieldIterator<FProperty> PropertyIt(ObjectClass); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		if ((ExcludedPropertyNames && ExcludedPropertyNames->Contains(Property->GetFName()))
			|| !IsWritableProperty(Property))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		if (bSkipDefaults && DefaultObject)
		{
			const void* DefaultValuePtr = Property->ContainerPtrToValuePtr<void>(DefaultObject);
			if (Property->Identical(ValuePtr, DefaultValuePtr))
			{
				continue;
			}
		}

		TSharedPtr<FJsonValue> JsonValue = ExtractPropertyValue(Property, ValuePtr);
		if (JsonValue.IsValid())
		{
			PropertiesJson->SetField(Property->GetName(), JsonValue);
		}
	}

	return PropertiesJson;
}

TSharedPtr<FJsonObject> FAssetDocumentPropertyAdapter::InspectProperties(
	UClass* Class,
	UObject* CurrentObject,
	const TSet<FName>* ExcludedPropertyNames)
{
	if (!Class)
	{
		return nullptr;
	}

	UObject* DefaultObject = Class->GetDefaultObject();
	UObject* ValueObject = CurrentObject ? CurrentObject : DefaultObject;
	if (!ValueObject || !DefaultObject)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("class"), Class->GetPathName());
	if (CurrentObject)
	{
		Payload->SetStringField(TEXT("asset_path"), CurrentObject->GetPathName());
	}

	TArray<TSharedPtr<FJsonValue>> PropertyRows;
	TArray<TSharedPtr<FJsonValue>> SkippedRows;

	for (TFieldIterator<FProperty> PropertyIt(Class); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		const FString PropertyName = Property->GetName();
		if (ExcludedPropertyNames && ExcludedPropertyNames->Contains(Property->GetFName()))
		{
			continue;
		}

		auto AddSkipped = [&SkippedRows, &PropertyName](const FString& Reason)
		{
			TSharedPtr<FJsonObject> Skipped = MakeShared<FJsonObject>();
			Skipped->SetStringField(TEXT("name"), PropertyName);
			Skipped->SetStringField(TEXT("reason"), Reason);
			SkippedRows.Add(MakeShared<FJsonValueObject>(Skipped));
		};

		const FString NonWritableReason = GetNonWritableReason(Property);
		if (!NonWritableReason.IsEmpty() && NonWritableReason != TEXT("edit-const"))
		{
			AddSkipped(NonWritableReason);
			continue;
		}

		const void* CurrentValuePtr = Property->ContainerPtrToValuePtr<void>(ValueObject);
		const void* DefaultValuePtr = Property->ContainerPtrToValuePtr<void>(DefaultObject);
		TSharedPtr<FJsonValue> CurrentValue = ExtractPropertyValue(Property, CurrentValuePtr);
		TSharedPtr<FJsonValue> DefaultValue = ExtractPropertyValue(Property, DefaultValuePtr);
		if (!CurrentValue.IsValid() || !DefaultValue.IsValid())
		{
			AddSkipped(TEXT("unsupported-serialization"));
			continue;
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), PropertyName);
		Row->SetStringField(TEXT("ue_type"), Property->GetCPPType());
		Row->SetStringField(TEXT("type_token"), GetTypeToken(Property));
		Row->SetField(TEXT("current_value"), CurrentValue);
		Row->SetField(TEXT("default_value"), DefaultValue);
		Row->SetBoolField(TEXT("writable"), IsWritableProperty(Property));
		PropertyRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	Payload->SetArrayField(TEXT("properties"), PropertyRows);
	Payload->SetArrayField(TEXT("skipped"), SkippedRows);
	TArray<TSharedPtr<FJsonValue>> SelectorPolicies;
	for (TFieldIterator<FProperty> PropertyIt(Class); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		CollectSelectorPoliciesFromValue(
			Property,
			Property->ContainerPtrToValuePtr<void>(ValueObject),
			Property->GetName(),
			SelectorPolicies);
	}
	Payload->SetArrayField(TEXT("selector_policies"), SelectorPolicies);
	return Payload;
}

FAssetDocumentPropertyApplyResult FAssetDocumentPropertyAdapter::ApplyProperties(UObject* Asset, TSharedPtr<FJsonObject> Properties)
{
	FAssetDocumentPropertyApplyResult Result;
	if (!Asset)
	{
		Result.Message = TEXT("Asset is required");
		return Result;
	}

	if (!Properties.IsValid() || Properties->Values.Num() == 0)
	{
		Result.bSuccess = true;
		Result.Message = TEXT("No properties to apply");
		return Result;
	}

	UObject* PreflightAsset = DuplicateObject<UObject>(Asset, GetTransientPackage());
	if (!PreflightAsset)
	{
		Result.Message = TEXT("Failed to create transient preflight asset");
		return Result;
	}

	if (!ApplyPropertiesDirect(PreflightAsset, Properties, Result.Diagnostics))
	{
		Result.Message = Result.Diagnostics.Num() > 0 ? Result.Diagnostics[0].Message : TEXT("Property preflight failed");
		return Result;
	}

	if (!ApplyPropertiesDirect(Asset, Properties, Result.Diagnostics))
	{
		Result.Message = Result.Diagnostics.Num() > 0 ? Result.Diagnostics.Last().Message : TEXT("Property apply failed");
		return Result;
	}

	Result.bSuccess = true;
	Result.Message = TEXT("Properties applied");
	return Result;
}

FAssetDocumentPropertyApplyResult FAssetDocumentPropertyAdapter::PreflightProperties(UClass* Class, TSharedPtr<FJsonObject> Properties)
{
	FAssetDocumentPropertyApplyResult Result;
	if (!Class)
	{
		Result.Message = TEXT("Class is required for property preflight");
		return Result;
	}

	if (!Properties.IsValid() || Properties->Values.Num() == 0)
	{
		Result.bSuccess = true;
		Result.Message = TEXT("No properties to preflight");
		return Result;
	}

	UObject* PreflightAsset = NewObject<UObject>(GetTransientPackage(), Class);
	if (!PreflightAsset)
	{
		Result.Message = FString::Printf(TEXT("Failed to create transient preflight object for '%s'"), *Class->GetName());
		return Result;
	}

	if (!ApplyPropertiesDirect(PreflightAsset, Properties, Result.Diagnostics))
	{
		Result.Message = Result.Diagnostics.Num() > 0 ? Result.Diagnostics[0].Message : TEXT("Property preflight failed");
		return Result;
	}

	Result.bSuccess = true;
	Result.Message = TEXT("Properties preflighted");
	return Result;
}

bool FAssetDocumentPropertyAdapter::ApplyPropertiesDirect(UObject* Asset, TSharedPtr<FJsonObject> Properties, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	bool bAllSucceeded = true;
	for (const auto& Pair : Properties->Values)
	{
		bAllSucceeded &= ApplySingleProperty(Asset, FString(*Pair.Key), Pair.Value, OutDiagnostics);
	}
	return bAllSucceeded;
}

bool FAssetDocumentPropertyAdapter::ApplySingleProperty(UObject* Asset, const FString& PropertyName, TSharedPtr<FJsonValue> JsonValue, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	if (!JsonValue.IsValid())
	{
		AddDiagnostic(OutDiagnostics, PropertyName, TEXT("InvalidValue"), TEXT("Property value is invalid"));
		return false;
	}

	FProperty* Property = FindFProperty<FProperty>(Asset->GetClass(), *PropertyName);
	if (!Property)
	{
		AddDiagnostic(OutDiagnostics, PropertyName, TEXT("UnknownProperty"), FString::Printf(TEXT("Property '%s' does not exist"), *PropertyName));
		return false;
	}

	const FString NonWritableReason = GetNonWritableReason(Property);
	if (!NonWritableReason.IsEmpty())
	{
		AddDiagnostic(OutDiagnostics, PropertyName, TEXT("NonWritable"), FString::Printf(TEXT("Property '%s' is not writable: %s"), *PropertyName, *NonWritableReason));
		return false;
	}

	bool bIsTyped = false;
	FString TypeName;
	FString TypeError;
	TSharedPtr<FJsonValue> ValueToApply = JsonValue;
	if (!TryGetTypedValue(JsonValue, TypeName, ValueToApply, bIsTyped, TypeError))
	{
		AddDiagnostic(OutDiagnostics, PropertyName, TEXT("InvalidTypedValue"), TypeError);
		return false;
	}

	if (bIsTyped && TypeName.Contains(TEXT(":")))
	{
		AddDiagnostic(OutDiagnostics, PropertyName, TEXT("UnsupportedTypedSubtype"), FString::Printf(TEXT("Typed property type '%s' with subtype is not supported by sidecar v1"), *TypeName));
		return false;
	}

	FString TypedValidationError;
	if (bIsTyped && !ValidateTypedValueForProperty(Property, TypeName, ValueToApply, TypedValidationError))
	{
		AddDiagnostic(OutDiagnostics, PropertyName, TEXT("TypedTypeMismatch"), TypedValidationError);
		return false;
	}

	if (!FPropertySetterUtils::SetPropertyFromJson(Asset, Property, ValueToApply))
	{
		AddDiagnostic(OutDiagnostics, PropertyName, TEXT("SetPropertyFailed"), FString::Printf(TEXT("Failed to set property '%s'"), *PropertyName));
		return false;
	}

	return true;
}

bool FAssetDocumentPropertyAdapter::TryGetTypedValue(TSharedPtr<FJsonValue> JsonValue, FString& OutType, TSharedPtr<FJsonValue>& OutValue, bool& bOutIsTyped, FString& OutError)
{
	bOutIsTyped = false;
	if (JsonValue->Type != EJson::Object)
	{
		return true;
	}

	TSharedPtr<FJsonObject> ValueObject = JsonValue->AsObject();
	if (!ValueObject.IsValid())
	{
		return true;
	}

	const bool bHasType = ValueObject->HasField(TEXT("type"));
	const bool bHasValue = ValueObject->HasField(TEXT("value"));
	if (!bHasType && !bHasValue)
	{
		return true;
	}

	bOutIsTyped = true;
	if (!ValueObject->TryGetStringField(TEXT("type"), OutType) || OutType.IsEmpty())
	{
		OutError = TEXT("Typed property requires a non-empty string 'type'");
		return false;
	}

	if (!bHasValue)
	{
		OutError = TEXT("Typed property requires a 'value' field");
		return false;
	}

	OutValue = ValueObject->TryGetField(TEXT("value"));
	if (!OutValue.IsValid())
	{
		OutError = TEXT("Typed property value field is invalid");
		return false;
	}

	return true;
}

bool FAssetDocumentPropertyAdapter::ValidateTypedValueForProperty(FProperty* Property, const FString& TypeName, TSharedPtr<FJsonValue> Value, FString& OutError)
{
	if (!Property)
	{
		OutError = TEXT("Typed property validation requires a target property");
		return false;
	}

	if (!Value.IsValid())
	{
		OutError = FString::Printf(TEXT("Typed property '%s' has an invalid value"), *Property->GetName());
		return false;
	}

	if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		if (!IsTypeName(TypeName, { TEXT("Bool"), TEXT("Boolean") }) || Value->Type != EJson::Boolean)
		{
			OutError = FString::Printf(TEXT("Typed property '%s' type '%s' is not compatible with bool property '%s'"), *Property->GetName(), *TypeName, *BoolProperty->GetName());
			return false;
		}
		return true;
	}

	if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
	{
		const bool bTypedAsInteger = IsTypeName(TypeName, { TEXT("Int"), TEXT("Integer"), TEXT("Int32") });
		const bool bTypedAsFloat = IsTypeName(TypeName, { TEXT("Float"), TEXT("Double"), TEXT("Number") });
		const bool bPropertyIsFloat = NumericProperty->IsFloatingPoint();
		const bool bTypeCompatible = bPropertyIsFloat ? bTypedAsFloat : bTypedAsInteger;
		if (!bTypeCompatible || Value->Type != EJson::Number)
		{
			OutError = FString::Printf(TEXT("Typed property '%s' type '%s' is not compatible with numeric property '%s'"), *Property->GetName(), *TypeName, *NumericProperty->GetName());
			return false;
		}
		return true;
	}

	if (CastField<FStrProperty>(Property))
	{
		if (!IsTypeName(TypeName, { TEXT("String") }) || Value->Type != EJson::String)
		{
			OutError = FString::Printf(TEXT("Typed property '%s' type '%s' is not compatible with string property '%s'"), *Property->GetName(), *TypeName, *Property->GetName());
			return false;
		}
		return true;
	}

	if (CastField<FNameProperty>(Property))
	{
		if (!IsTypeName(TypeName, { TEXT("Name"), TEXT("String") }) || Value->Type != EJson::String)
		{
			OutError = FString::Printf(TEXT("Typed property '%s' type '%s' is not compatible with name property '%s'"), *Property->GetName(), *TypeName, *Property->GetName());
			return false;
		}
		return true;
	}

	if (CastField<FTextProperty>(Property))
	{
		if (!IsTypeName(TypeName, { TEXT("Text"), TEXT("String") }) || Value->Type != EJson::String)
		{
			OutError = FString::Printf(TEXT("Typed property '%s' type '%s' is not compatible with text property '%s'"), *Property->GetName(), *TypeName, *Property->GetName());
			return false;
		}
		return true;
	}

	OutError = FString::Printf(TEXT("Typed property '%s' type '%s' is not supported for sidecar v1 validation"), *Property->GetName(), *TypeName);
	return false;
}

void FAssetDocumentPropertyAdapter::AddDiagnostic(TArray<FAssetDocumentDiagnostic>& Diagnostics, const FString& PropertyName, const FString& Code, const FString& Message)
{
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = FString::Printf(TEXT("Properties.%s"), *PropertyName);
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostics.Add(Diagnostic);
}
