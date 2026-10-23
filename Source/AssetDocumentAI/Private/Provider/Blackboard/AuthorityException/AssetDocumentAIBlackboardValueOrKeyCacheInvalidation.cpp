// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardValueOrKeyCacheInvalidation.h"

#include "BehaviorTree/ValueOrBBKey.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/StructView.h"
#include "UObject/UnrealType.h"


namespace AIBlackboardValueOrKeyCacheInvalidationLocal
{
using namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation;
struct FClassAuthoredFingerprint
{
	FName Key;
	UObject* DefaultValue = nullptr;
	UObject* BaseClass = nullptr;

	bool operator==(const FClassAuthoredFingerprint& Other) const
	{
		return Key == Other.Key && DefaultValue == Other.DefaultValue && BaseClass == Other.BaseClass;
	}
};

struct FEnumAuthoredFingerprint
{
	FName Key;
	uint8 DefaultValue = 0;
	UObject* EnumType = nullptr;
	FString NativeEnumTypeName;

	bool operator==(const FEnumAuthoredFingerprint& Other) const
	{
		return Key == Other.Key && DefaultValue == Other.DefaultValue
			&& EnumType == Other.EnumType && NativeEnumTypeName == Other.NativeEnumTypeName;
	}
};

struct FStructAuthoredFingerprint
{
	FName Key;
	FInstancedStruct DefaultValue;
	bool bCanEditDefaultValueType = false;

	bool operator==(const FStructAuthoredFingerprint& Other) const
	{
		return Key == Other.Key && DefaultValue == Other.DefaultValue
			&& bCanEditDefaultValueType == Other.bCanEditDefaultValueType;
	}
};

struct FClassOrObjectSchema
{
	FObjectProperty* DefaultValue = nullptr;
	FObjectProperty* BaseClass = nullptr;
};

struct FEnumSchema
{
	FByteProperty* DefaultValue = nullptr;
	FObjectProperty* EnumType = nullptr;
	FStrProperty* NativeEnumTypeName = nullptr;
};

struct FStructSchema
{
	FStructProperty* DefaultValue = nullptr;
	FBoolProperty* CanEditDefaultValueType = nullptr;
};

bool HasExactPropertyIdentity(
	const FProperty& Property,
	const UScriptStruct& Owner,
	const FName ExpectedName,
	const FFieldClass* ExpectedClass)
{
	return Property.GetOwnerStruct() == &Owner
		&& Property.GetFName() == ExpectedName
		&& Property.GetClass() == ExpectedClass;
}

bool ResolveClassOrObjectSchema(const UScriptStruct& Owner, FClassOrObjectSchema& OutSchema)
{
	const bool bClassFamily = &Owner == FValueOrBBKey_Class::StaticStruct();
	const bool bObjectFamily = &Owner == FValueOrBBKey_Object::StaticStruct();
	if (!bClassFamily && !bObjectFamily)
	{
		return false;
	}
	const FFieldClass* ExpectedDefaultClass = bClassFamily
		? FClassProperty::StaticClass() : FObjectProperty::StaticClass();
	int32 OwnPropertyCount = 0;
	for (TFieldIterator<FProperty> It(&Owner, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		++OwnPropertyCount;
		FProperty& Property = **It;
		if (Property.GetFName() == TEXT("DefaultValue"))
		{
			if (!HasExactPropertyIdentity(Property, Owner, TEXT("DefaultValue"), ExpectedDefaultClass))
			{
				return false;
			}
			OutSchema.DefaultValue = CastField<FObjectProperty>(&Property);
		}
		else if (Property.GetFName() == TEXT("BaseClass"))
		{
			if (!HasExactPropertyIdentity(Property, Owner, TEXT("BaseClass"), FClassProperty::StaticClass()))
			{
				return false;
			}
			OutSchema.BaseClass = CastField<FObjectProperty>(&Property);
		}
	}
	return OwnPropertyCount == 2 && OutSchema.DefaultValue != nullptr && OutSchema.BaseClass != nullptr;
}

bool ResolveEnumSchema(const UScriptStruct& Owner, FEnumSchema& OutSchema)
{
	int32 OwnPropertyCount = 0;
	for (TFieldIterator<FProperty> It(&Owner, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		++OwnPropertyCount;
		FProperty& Property = **It;
		if (Property.GetFName() == TEXT("DefaultValue"))
		{
			if (!HasExactPropertyIdentity(Property, Owner, TEXT("DefaultValue"), FByteProperty::StaticClass()))
			{
				return false;
			}
			OutSchema.DefaultValue = CastField<FByteProperty>(&Property);
		}
		else if (Property.GetFName() == TEXT("EnumType"))
		{
			if (!HasExactPropertyIdentity(Property, Owner, TEXT("EnumType"), FObjectProperty::StaticClass()))
			{
				return false;
			}
			OutSchema.EnumType = CastField<FObjectProperty>(&Property);
		}
		else if (Property.GetFName() == TEXT("NativeEnumTypeName"))
		{
			if (!HasExactPropertyIdentity(Property, Owner, TEXT("NativeEnumTypeName"), FStrProperty::StaticClass()))
			{
				return false;
			}
			OutSchema.NativeEnumTypeName = CastField<FStrProperty>(&Property);
		}
	}
	return OwnPropertyCount == 3 && OutSchema.DefaultValue != nullptr && OutSchema.EnumType != nullptr
		&& OutSchema.NativeEnumTypeName != nullptr;
}

bool ResolveStructSchema(const UScriptStruct& Owner, FStructSchema& OutSchema)
{
	int32 OwnPropertyCount = 0;
	for (TFieldIterator<FProperty> It(&Owner, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		++OwnPropertyCount;
		FProperty& Property = **It;
		if (Property.GetFName() == TEXT("DefaultValue"))
		{
			if (!HasExactPropertyIdentity(Property, Owner, TEXT("DefaultValue"), FStructProperty::StaticClass()))
			{
				return false;
			}
			OutSchema.DefaultValue = CastField<FStructProperty>(&Property);
			if (OutSchema.DefaultValue == nullptr || OutSchema.DefaultValue->Struct != FInstancedStruct::StaticStruct())
			{
				return false;
			}
		}
		else if (Property.GetFName() == TEXT("bCanEditDefaultValueType"))
		{
			if (!HasExactPropertyIdentity(Property, Owner, TEXT("bCanEditDefaultValueType"), FBoolProperty::StaticClass()))
			{
				return false;
			}
			OutSchema.CanEditDefaultValueType = CastField<FBoolProperty>(&Property);
		}
	}
	return OwnPropertyCount == 2 && OutSchema.DefaultValue != nullptr && OutSchema.CanEditDefaultValueType != nullptr;
}

FClassAuthoredFingerprint CaptureClassOrObjectFingerprint(
	const FValueOrBlackboardKeyBase& Leaf,
	const FClassOrObjectSchema& Schema)
{
	return { Leaf.GetKey(), Schema.DefaultValue->GetObjectPropertyValue_InContainer(&Leaf),
		Schema.BaseClass->GetObjectPropertyValue_InContainer(&Leaf) };
}

FEnumAuthoredFingerprint CaptureEnumFingerprint(const FValueOrBBKey_Enum& Leaf, const FEnumSchema& Schema)
{
	return { Leaf.GetKey(), Schema.DefaultValue->GetPropertyValue_InContainer(&Leaf),
		Schema.EnumType->GetObjectPropertyValue_InContainer(&Leaf),
		Schema.NativeEnumTypeName->GetPropertyValue_InContainer(&Leaf) };
}

FStructAuthoredFingerprint CaptureStructFingerprint(const FValueOrBBKey_Struct& Leaf, const FStructSchema& Schema)
{
	return { Leaf.GetKey(), *Schema.DefaultValue->ContainerPtrToValuePtr<FInstancedStruct>(&Leaf),
		Schema.CanEditDefaultValueType->GetPropertyValue_InContainer(&Leaf) };
}

#if WITH_DEV_AUTOMATION_TESTS
EValueOrKeyCacheInvalidationTestFailurePoint GFailurePoint =
	EValueOrKeyCacheInvalidationTestFailurePoint::None;

bool ShouldFail(const EValueOrKeyCacheInvalidationTestFailurePoint Point)
{
	return GFailurePoint == Point;
}
#else
bool ShouldFail(const EValueOrKeyCacheInvalidationTestFailurePoint)
{
	return false;
}
#endif

template <typename TLeaf, typename TValue>
EExactValueOrKeyCacheInvalidationResult RebuildSimple(
	TLeaf& Live,
	const UScriptStruct& DeclaredStruct,
	UScriptStruct* ExpectedStruct,
	const EValueOrKeyCacheInvalidationTestFailurePoint BeforeFailurePoint,
	const EValueOrKeyCacheInvalidationTestFailurePoint AfterFailurePoint)
{
	if (&DeclaredStruct != ExpectedStruct)
	{
		return EExactValueOrKeyCacheInvalidationResult::UnsupportedExactFamily;
	}
	if (ShouldFail(BeforeFailurePoint))
	{
		return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	}
	const TValue DefaultValue = Live.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
	const FName Key = Live.GetKey();
	const auto BeforeFingerprint = MakeTuple(Key, DefaultValue);
	TLeaf Fresh(DefaultValue);
	Fresh.SetKey(Key);
	if (ShouldFail(AfterFailurePoint))
	{
		return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	}
	const auto AfterFingerprint = MakeTuple(
		Fresh.GetKey(),
		Fresh.GetValue(static_cast<const UBlackboardComponent*>(nullptr)));
	if (BeforeFingerprint != AfterFingerprint)
	{
		return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	}
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeFinalAssignment))
	{
		return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	}
	Live = MoveTemp(Fresh);
	return EExactValueOrKeyCacheInvalidationResult::Success;
}

EExactValueOrKeyCacheInvalidationResult RebuildClass(
	FValueOrBBKey_Class& Live, const UScriptStruct& DeclaredStruct)
{
	if (&DeclaredStruct != FValueOrBBKey_Class::StaticStruct()) return EExactValueOrKeyCacheInvalidationResult::UnsupportedExactFamily;
	FClassOrObjectSchema Schema;
	if (!ResolveClassOrObjectSchema(DeclaredStruct, Schema)) return EExactValueOrKeyCacheInvalidationResult::SchemaMismatch;
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeClass)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	UClass* const PublicDefaultValue = Live.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
	const FClassAuthoredFingerprint Before = CaptureClassOrObjectFingerprint(Live, Schema);
	if (PublicDefaultValue != Before.DefaultValue) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	UClass* const BaseClass = Cast<UClass>(Before.BaseClass);
	if (BaseClass == nullptr) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	FValueOrBBKey_Class Fresh;
	Fresh.SetBaseClass(BaseClass);
	Schema.DefaultValue->SetObjectPropertyValue_InContainer(&Fresh, Before.DefaultValue);
	Fresh.SetKey(Before.Key);
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::AfterClass)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	if (!(Before == CaptureClassOrObjectFingerprint(Fresh, Schema))) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeFinalAssignment)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	Live = MoveTemp(Fresh);
	return EExactValueOrKeyCacheInvalidationResult::Success;
}

EExactValueOrKeyCacheInvalidationResult RebuildObject(
	FValueOrBBKey_Object& Live, const UScriptStruct& DeclaredStruct)
{
	if (&DeclaredStruct != FValueOrBBKey_Object::StaticStruct()) return EExactValueOrKeyCacheInvalidationResult::UnsupportedExactFamily;
	FClassOrObjectSchema Schema;
	if (!ResolveClassOrObjectSchema(DeclaredStruct, Schema)) return EExactValueOrKeyCacheInvalidationResult::SchemaMismatch;
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeObject)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	UObject* const PublicDefaultValue = Live.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
	const FClassAuthoredFingerprint Before = CaptureClassOrObjectFingerprint(Live, Schema);
	if (PublicDefaultValue != Before.DefaultValue) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	UClass* const BaseClass = Cast<UClass>(Before.BaseClass);
	if (BaseClass == nullptr) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	FValueOrBBKey_Object Fresh;
	Fresh.SetBaseClass(BaseClass);
	Schema.DefaultValue->SetObjectPropertyValue_InContainer(&Fresh, Before.DefaultValue);
	Fresh.SetKey(Before.Key);
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::AfterObject)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	if (!(Before == CaptureClassOrObjectFingerprint(Fresh, Schema))) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeFinalAssignment)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	Live = MoveTemp(Fresh);
	return EExactValueOrKeyCacheInvalidationResult::Success;
}

EExactValueOrKeyCacheInvalidationResult RebuildEnum(FValueOrBBKey_Enum& Live, const UScriptStruct& DeclaredStruct)
{
	if (&DeclaredStruct != FValueOrBBKey_Enum::StaticStruct()) return EExactValueOrKeyCacheInvalidationResult::UnsupportedExactFamily;
	FEnumSchema Schema;
	if (!ResolveEnumSchema(DeclaredStruct, Schema)) return EExactValueOrKeyCacheInvalidationResult::SchemaMismatch;
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeEnum)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	const uint8 PublicDefaultValue = Live.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
	const FEnumAuthoredFingerprint Before = CaptureEnumFingerprint(Live, Schema);
	if (PublicDefaultValue != Before.DefaultValue) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	FValueOrBBKey_Enum Fresh;
	Fresh.SetEnumType(Cast<UEnum>(Before.EnumType));
	Schema.DefaultValue->SetPropertyValue_InContainer(&Fresh, Before.DefaultValue);
	Schema.NativeEnumTypeName->SetPropertyValue_InContainer(&Fresh, Before.NativeEnumTypeName);
	Fresh.SetKey(Before.Key);
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::AfterEnum)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	if (!(Before == CaptureEnumFingerprint(Fresh, Schema))) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeFinalAssignment)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	Live = MoveTemp(Fresh);
	return EExactValueOrKeyCacheInvalidationResult::Success;
}

EExactValueOrKeyCacheInvalidationResult RebuildStruct(FValueOrBBKey_Struct& Live, const UScriptStruct& DeclaredStruct)
{
	if (&DeclaredStruct != FValueOrBBKey_Struct::StaticStruct()) return EExactValueOrKeyCacheInvalidationResult::UnsupportedExactFamily;
	FStructSchema Schema;
	if (!ResolveStructSchema(DeclaredStruct, Schema)) return EExactValueOrKeyCacheInvalidationResult::SchemaMismatch;
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeStruct)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	const FConstStructView PublicDefaultValue = Live.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
	const FStructAuthoredFingerprint Before = CaptureStructFingerprint(Live, Schema);
	const UScriptStruct* const CapturedStructType = Before.DefaultValue.GetScriptStruct();
	if (PublicDefaultValue.GetScriptStruct() != CapturedStructType) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	FValueOrBBKey_Struct Fresh;
	if (CapturedStructType != nullptr) Fresh.SetStructType(const_cast<UScriptStruct*>(CapturedStructType));
	*Schema.DefaultValue->ContainerPtrToValuePtr<FInstancedStruct>(&Fresh) = Before.DefaultValue;
	Schema.CanEditDefaultValueType->SetPropertyValue_InContainer(&Fresh, Before.bCanEditDefaultValueType);
	Fresh.SetKey(Before.Key);
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::AfterStruct)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	if (!(Before == CaptureStructFingerprint(Fresh, Schema))) return EExactValueOrKeyCacheInvalidationResult::AuthoredFingerprintMismatch;
	if (ShouldFail(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeFinalAssignment)) return EExactValueOrKeyCacheInvalidationResult::TestFailure;
	Live = MoveTemp(Fresh);
	return EExactValueOrKeyCacheInvalidationResult::Success;
}
} // namespace

namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation
{
#define ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(Type, Name) \
	FResolvedExactValueOrLeaf::FResolvedExactValueOrLeaf(Type& InLeaf, UScriptStruct& InDeclaredStruct) \
		: FResolvedExactValueOrLeaf(FLeafReference(TInPlaceType<Type*>(), &InLeaf), InDeclaredStruct, EExactValueOrBlackboardKeyFamily::Name) {}
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Bool, Bool)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Class, Class)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Enum, Enum)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Float, Float)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Int32, Int32)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Name, Name)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_String, String)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Object, Object)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Rotator, Rotator)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Struct, Struct)
ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF(FValueOrBBKey_Vector, Vector)
#undef ASSETDOCUMENT_DEFINE_RESOLVED_VALUE_OR_LEAF

FResolvedExactValueOrLeaf::FResolvedExactValueOrLeaf(
	FLeafReference InLeaf,
	UScriptStruct& InDeclaredStruct,
	const EExactValueOrBlackboardKeyFamily InFamily)
	: Leaf(MoveTemp(InLeaf))
	, DeclaredStruct(&InDeclaredStruct)
	, Family(InFamily)
{
}

EExactValueOrKeyCacheInvalidationResult InvalidateExactValueOrBlackboardKeyCache(
	FResolvedExactValueOrLeaf& ResolvedLeaf)
{
	using namespace AIBlackboardValueOrKeyCacheInvalidationLocal;
	if (ResolvedLeaf.DeclaredStruct == nullptr)
	{
		return EExactValueOrKeyCacheInvalidationResult::InvalidResolvedLeaf;
	}

	switch (ResolvedLeaf.Family)
	{
	case EExactValueOrBlackboardKeyFamily::Bool:
		if (FValueOrBBKey_Bool** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Bool*>()) return RebuildSimple<FValueOrBBKey_Bool, bool>(**Leaf, *ResolvedLeaf.DeclaredStruct, FValueOrBBKey_Bool::StaticStruct(), EValueOrKeyCacheInvalidationTestFailurePoint::BeforeBool, EValueOrKeyCacheInvalidationTestFailurePoint::AfterBool);
		break;
	case EExactValueOrBlackboardKeyFamily::Class:
		if (FValueOrBBKey_Class** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Class*>()) return RebuildClass(**Leaf, *ResolvedLeaf.DeclaredStruct);
		break;
	case EExactValueOrBlackboardKeyFamily::Enum:
		if (FValueOrBBKey_Enum** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Enum*>()) return RebuildEnum(**Leaf, *ResolvedLeaf.DeclaredStruct);
		break;
	case EExactValueOrBlackboardKeyFamily::Float:
		if (FValueOrBBKey_Float** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Float*>()) return RebuildSimple<FValueOrBBKey_Float, float>(**Leaf, *ResolvedLeaf.DeclaredStruct, FValueOrBBKey_Float::StaticStruct(), EValueOrKeyCacheInvalidationTestFailurePoint::BeforeFloat, EValueOrKeyCacheInvalidationTestFailurePoint::AfterFloat);
		break;
	case EExactValueOrBlackboardKeyFamily::Int32:
		if (FValueOrBBKey_Int32** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Int32*>()) return RebuildSimple<FValueOrBBKey_Int32, int32>(**Leaf, *ResolvedLeaf.DeclaredStruct, FValueOrBBKey_Int32::StaticStruct(), EValueOrKeyCacheInvalidationTestFailurePoint::BeforeInt32, EValueOrKeyCacheInvalidationTestFailurePoint::AfterInt32);
		break;
	case EExactValueOrBlackboardKeyFamily::Name:
		if (FValueOrBBKey_Name** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Name*>()) return RebuildSimple<FValueOrBBKey_Name, FName>(**Leaf, *ResolvedLeaf.DeclaredStruct, FValueOrBBKey_Name::StaticStruct(), EValueOrKeyCacheInvalidationTestFailurePoint::BeforeName, EValueOrKeyCacheInvalidationTestFailurePoint::AfterName);
		break;
	case EExactValueOrBlackboardKeyFamily::String:
		if (FValueOrBBKey_String** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_String*>()) return RebuildSimple<FValueOrBBKey_String, FString>(**Leaf, *ResolvedLeaf.DeclaredStruct, FValueOrBBKey_String::StaticStruct(), EValueOrKeyCacheInvalidationTestFailurePoint::BeforeString, EValueOrKeyCacheInvalidationTestFailurePoint::AfterString);
		break;
	case EExactValueOrBlackboardKeyFamily::Object:
		if (FValueOrBBKey_Object** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Object*>()) return RebuildObject(**Leaf, *ResolvedLeaf.DeclaredStruct);
		break;
	case EExactValueOrBlackboardKeyFamily::Rotator:
		if (FValueOrBBKey_Rotator** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Rotator*>()) return RebuildSimple<FValueOrBBKey_Rotator, FRotator>(**Leaf, *ResolvedLeaf.DeclaredStruct, FValueOrBBKey_Rotator::StaticStruct(), EValueOrKeyCacheInvalidationTestFailurePoint::BeforeRotator, EValueOrKeyCacheInvalidationTestFailurePoint::AfterRotator);
		break;
	case EExactValueOrBlackboardKeyFamily::Struct:
		if (FValueOrBBKey_Struct** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Struct*>()) return RebuildStruct(**Leaf, *ResolvedLeaf.DeclaredStruct);
		break;
	case EExactValueOrBlackboardKeyFamily::Vector:
		if (FValueOrBBKey_Vector** Leaf = ResolvedLeaf.Leaf.TryGet<FValueOrBBKey_Vector*>()) return RebuildSimple<FValueOrBBKey_Vector, FVector>(**Leaf, *ResolvedLeaf.DeclaredStruct, FValueOrBBKey_Vector::StaticStruct(), EValueOrKeyCacheInvalidationTestFailurePoint::BeforeVector, EValueOrKeyCacheInvalidationTestFailurePoint::AfterVector);
		break;
	}
	return EExactValueOrKeyCacheInvalidationResult::InvalidResolvedLeaf;
}

#if WITH_DEV_AUTOMATION_TESTS
void SetValueOrKeyCacheInvalidationTestFailurePoint(
	const EValueOrKeyCacheInvalidationTestFailurePoint FailurePoint)
{
	using namespace AIBlackboardValueOrKeyCacheInvalidationLocal;
	GFailurePoint = FailurePoint;
}
#endif // WITH_DEV_AUTOMATION_TESTS
} // namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation
