// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h"

#include "TypedPreimage/AssetDocumentAITypedPreimageInternal.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/BlackboardData.h"
#include "IDetailTreeNode.h"
#include "IDetailCustomization.h"
#include "IPropertyRowGenerator.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "StructUtils/InstancedStruct.h"
#include "Misc/TVariant.h"
#include "UObject/Package.h"
#include "UObject/GCObject.h"
#include "UObject/PropertyOptional.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include <charconv>

namespace UE::AssetDocument::AI::TypedPreimage::Blackboard
{
using namespace UE::AssetDocument::AI::TypedPreimage;

constexpr const TCHAR* KeyTypePath =
	TEXT("/AITypedPreimage/BlackboardKeyType");
constexpr const TCHAR* LocalKeysPath =
	TEXT("/AITypedPreimage/BlackboardLocalKeys");
constexpr EPropertyValueSetFlags::Type RestoreFlags =
	EPropertyValueSetFlags::NotTransactable;

using FLeafValue = TVariant<
	bool,
	int8,
	int16,
	int32,
	int64,
	uint8,
	uint16,
	uint32,
	uint64,
	float,
	double,
	FString,
	FText,
	FName,
	FVector2D,
	FVector,
	FVector4,
	FQuat,
	FRotator>;

enum class ELeafKind : uint8
{
	Bool,
	Int8,
	Int16,
	Int32,
	Int64,
	UInt8,
	UInt16,
	UInt32,
	UInt64,
	Float,
	Double,
	String,
	Text,
	Name,
	Vector2D,
	Vector,
	Vector4,
	Quat,
	Rotator,
	Object,
	InstancedStruct,
};

bool DoesGeneratorSelectExactOwner(
	const IPropertyRowGenerator& Rows,
	const UObject& Owner)
{
	const TArray<TWeakObjectPtr<UObject>>& Selected = Rows.GetSelectedObjects();
	return Selected.Num() == 1 && Selected[0].Get() == &Owner;
}

bool HasExactOuterObject(
	const IPropertyHandle& Handle,
	const UObject& Owner)
{
	TArray<UObject*> Outers;
	Handle.GetOuterObjects(Outers);
	return Outers.Num() == 1 && Outers[0] == &Owner;
}

bool IsForbiddenPropertyFamily(const FProperty& Property)
{
	return Property.GetOwnerProperty() != &Property
		|| Property.HasAnyPropertyFlags(CPF_InstancedReference)
		|| CastField<FArrayProperty>(&Property) != nullptr
		|| CastField<FSetProperty>(&Property) != nullptr
		|| CastField<FMapProperty>(&Property) != nullptr
		|| CastField<FOptionalProperty>(&Property) != nullptr
		|| CastField<FDelegateProperty>(&Property) != nullptr
		|| CastField<FMulticastDelegateProperty>(&Property) != nullptr
		|| CastField<FInterfaceProperty>(&Property) != nullptr
		|| CastField<FSoftObjectProperty>(&Property) != nullptr
		|| CastField<FSoftClassProperty>(&Property) != nullptr
		|| CastField<FWeakObjectProperty>(&Property) != nullptr
		|| CastField<FLazyObjectProperty>(&Property) != nullptr;
}

bool IsAuthoredPropertyHandle(
	const IPropertyHandle& Handle,
	const FProperty& Property)
{
	return Handle.IsValidHandle()
		&& Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_Transient | CPF_EditConst);
}

bool IsEditableAuthoredProperty(
	const IPropertyHandle& Handle,
	const FProperty& Property)
{
	return IsAuthoredPropertyHandle(Handle, Property)
		&& Handle.IsEditable();
}

template <typename T>
bool CaptureValue(
	const IPropertyHandle& Handle,
	FLeafValue& OutValue)
{
	T Value{};
	if (Handle.GetValue(Value) != FPropertyAccess::Success)
	{
		return false;
	}
	OutValue.Set<T>(MoveTemp(Value));
	return true;
}

template <typename T>
bool RestoreValue(
	IPropertyHandle& Handle,
	const FLeafValue& Value)
{
	return Handle.SetValue(Value.Get<T>(), RestoreFlags)
		== FPropertyAccess::Success;
}

template <typename T>
bool ReadbackValue(
	const IPropertyHandle& Handle,
	const FLeafValue& Value)
{
	T Current{};
	return Handle.GetValue(Current) == FPropertyAccess::Success
		&& Current == Value.Get<T>();
}

bool AreAuthoredTextsIdenticalForRollback(
	const FText& Left,
	const FText& Right)
{
	if (FTextInspector::GetFlags(Left) != FTextInspector::GetFlags(Right))
	{
		return false;
	}
	FName LeftTableId;
	FString LeftTableKey;
	FName RightTableId;
	FString RightTableKey;
	const bool bLeftStringTable = FTextInspector::GetTableIdAndKey(
		Left, LeftTableId, LeftTableKey);
	const bool bRightStringTable = FTextInspector::GetTableIdAndKey(
		Right, RightTableId, RightTableKey);
	if (bLeftStringTable || bRightStringTable)
	{
		return bLeftStringTable
			&& bRightStringTable
			&& LeftTableId == RightTableId
			&& LeftTableKey == RightTableKey;
	}
	const FTextId LeftId = FTextInspector::GetTextId(Left);
	const FTextId RightId = FTextInspector::GetTextId(Right);
	const bool bLeftHasId = !LeftId.GetNamespace().IsEmpty()
		|| !LeftId.GetKey().IsEmpty();
	const bool bRightHasId = !RightId.GetNamespace().IsEmpty()
		|| !RightId.GetKey().IsEmpty();
	if (bLeftHasId || bRightHasId)
	{
		const FString* LeftSource = FTextInspector::GetSourceString(Left);
		const FString* RightSource = FTextInspector::GetSourceString(Right);
		return bLeftHasId
			&& bRightHasId
			&& LeftId == RightId
			&& (LeftSource == nullptr) == (RightSource == nullptr)
			&& (LeftSource == nullptr || *LeftSource == *RightSource);
	}
	return Left.IdenticalTo(
		Right,
		ETextIdenticalModeFlags::DeepCompare
			| ETextIdenticalModeFlags::LexicalCompareInvariants);
}

bool HasOnlyFiniteComponents(const FVector2D& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
}

bool HasOnlyFiniteComponents(const FVector& Value)
{
	return FMath::IsFinite(Value.X)
		&& FMath::IsFinite(Value.Y)
		&& FMath::IsFinite(Value.Z);
}

bool HasOnlyFiniteComponents(const FVector4& Value)
{
	return FMath::IsFinite(Value.X)
		&& FMath::IsFinite(Value.Y)
		&& FMath::IsFinite(Value.Z)
		&& FMath::IsFinite(Value.W);
}

bool HasOnlyFiniteComponents(const FQuat& Value)
{
	return FMath::IsFinite(Value.X)
		&& FMath::IsFinite(Value.Y)
		&& FMath::IsFinite(Value.Z)
		&& FMath::IsFinite(Value.W);
}

bool HasOnlyFiniteComponents(const FRotator& Value)
{
	return FMath::IsFinite(Value.Pitch)
		&& FMath::IsFinite(Value.Yaw)
		&& FMath::IsFinite(Value.Roll);
}

template <typename T>
bool CaptureFiniteStructValue(
	const IPropertyHandle& Handle,
	FLeafValue& OutValue,
	bool& bOutValueNotFinite)
{
	T Value{};
	if (Handle.GetValue(Value) != FPropertyAccess::Success)
	{
		return false;
	}
	if (!HasOnlyFiniteComponents(Value))
	{
		bOutValueNotFinite = true;
		return false;
	}
	OutValue.Set<T>(MoveTemp(Value));
	return true;
}

struct FTypedLeaf
{
	ELeafKind Kind = ELeafKind::Bool;
	TFieldPath<FProperty> Property;
	FName PropertyName;
	FString PropertyPath;
	const FFieldClass* PropertyClass = nullptr;
	TWeakObjectPtr<UScriptStruct> StructType;
	TWeakObjectPtr<UClass> ObjectClass;
	FLeafValue Value;
	TStrongObjectPtr<UObject> ObjectValue;
	FInstancedStruct InstancedStructValue;
	bool bRequireEditableHandle = true;

	bool MatchesCurrent(
		const IPropertyHandle& Handle,
		const UObject& Owner) const
	{
		FProperty* Current = Handle.GetProperty();
		if (!Handle.IsValidHandle()
			|| (bRequireEditableHandle && !Handle.IsEditable())
			|| Current == nullptr
			|| Current != Property.Get()
			|| Current->GetClass() != PropertyClass
			|| Current->GetFName() != PropertyName
			|| Current->GetOwnerProperty() != Current
			|| Handle.GeneratePathToProperty() != PropertyPath
			|| !HasExactOuterObject(Handle, Owner))
		{
			return false;
		}
		if (const FStructProperty* StructProperty =
			CastField<FStructProperty>(Current))
		{
			return StructProperty->Struct == StructType.Get();
		}
		if (const FObjectPropertyBase* ObjectProperty =
			CastField<FObjectPropertyBase>(Current))
		{
			return ObjectProperty->PropertyClass == ObjectClass.Get();
		}
		return true;
	}
};

void GatherMatchingHandles(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	const FTypedLeaf& Leaf,
	const UObject& Owner,
	TArray<TSharedPtr<IPropertyHandle>>& OutMatches)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid() && Leaf.MatchesCurrent(*Handle, Owner))
		{
			OutMatches.Add(Handle);
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		GatherMatchingHandles(Children, Leaf, Owner, OutMatches);
	}
}

bool FindUniqueCurrentHandle(
	const IPropertyRowGenerator& Rows,
	const UObject& Owner,
	const FTypedLeaf& Leaf,
	TSharedPtr<IPropertyHandle>& OutHandle)
{
	if (!DoesGeneratorSelectExactOwner(Rows, Owner))
	{
		return false;
	}
	TArray<TSharedPtr<IPropertyHandle>> Matches;
	GatherMatchingHandles(Rows.GetRootTreeNodes(), Leaf, Owner, Matches);
	if (Matches.Num() != 1)
	{
		return false;
	}
	OutHandle = Matches[0];
	return OutHandle.IsValid();
}

bool CaptureLeaf(
	UBlackboardKeyType& KeyType,
	const TSharedRef<IPropertyRowGenerator>& Rows,
	const TSharedRef<IPropertyHandle>& Handle,
	bool bRequireEditableHandle,
	FTypedLeaf& OutLeaf,
	bool& bOutValueNotFinite)
{
	bOutValueNotFinite = false;
	const TSharedPtr<IPropertyHandle> HandlePtr = Handle;
	FProperty* Property = Handle->GetProperty();
	if (Property == nullptr
		|| IsForbiddenPropertyFamily(*Property)
		|| (bRequireEditableHandle
			? !IsEditableAuthoredProperty(*Handle, *Property)
			: !IsAuthoredPropertyHandle(*Handle, *Property))
		|| !Rows->FindTreeNode(HandlePtr).IsValid()
		|| !HasExactOuterObject(*Handle, KeyType)
		|| Property->GetOwnerClass() == nullptr
		|| !KeyType.IsA(Property->GetOwnerClass()))
	{
		return false;
	}
	const FString StablePath = Handle->GeneratePathToProperty();
	if (StablePath.IsEmpty())
	{
		return false;
	}
	OutLeaf.Property = Property;
	OutLeaf.PropertyName = Property->GetFName();
	OutLeaf.PropertyPath = StablePath;
	OutLeaf.PropertyClass = Property->GetClass();
	OutLeaf.bRequireEditableHandle = bRequireEditableHandle;

	if (CastField<FBoolProperty>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Bool;
		return CaptureValue<bool>(*Handle, OutLeaf.Value);
	}
	if (CastField<FInt8Property>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Int8;
		return CaptureValue<int8>(*Handle, OutLeaf.Value);
	}
	if (CastField<FInt16Property>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Int16;
		return CaptureValue<int16>(*Handle, OutLeaf.Value);
	}
	if (CastField<FIntProperty>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Int32;
		return CaptureValue<int32>(*Handle, OutLeaf.Value);
	}
	if (CastField<FInt64Property>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Int64;
		return CaptureValue<int64>(*Handle, OutLeaf.Value);
	}
	if (const FByteProperty* ByteProperty =
		CastField<FByteProperty>(Property);
		ByteProperty != nullptr
		&& ByteProperty->GetIntPropertyEnum() == nullptr)
	{
		OutLeaf.Kind = ELeafKind::UInt8;
		return CaptureValue<uint8>(*Handle, OutLeaf.Value);
	}
	if (CastField<FUInt16Property>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::UInt16;
		return CaptureValue<uint16>(*Handle, OutLeaf.Value);
	}
	if (CastField<FUInt32Property>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::UInt32;
		return CaptureValue<uint32>(*Handle, OutLeaf.Value);
	}
	if (CastField<FUInt64Property>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::UInt64;
		return CaptureValue<uint64>(*Handle, OutLeaf.Value);
	}
	if (CastField<FFloatProperty>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Float;
		return CaptureValue<float>(*Handle, OutLeaf.Value);
	}
	if (CastField<FDoubleProperty>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Double;
		return CaptureValue<double>(*Handle, OutLeaf.Value);
	}
	if (CastField<FStrProperty>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::String;
		return CaptureValue<FString>(*Handle, OutLeaf.Value);
	}
	if (CastField<FTextProperty>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Text;
		return CaptureValue<FText>(*Handle, OutLeaf.Value);
	}
	if (CastField<FNameProperty>(Property) != nullptr)
	{
		OutLeaf.Kind = ELeafKind::Name;
		return CaptureValue<FName>(*Handle, OutLeaf.Value);
	}
	if (const FStructProperty* StructProperty =
		CastField<FStructProperty>(Property))
	{
		OutLeaf.StructType = StructProperty->Struct;
		if (StructProperty->Struct == TBaseStructure<FVector2D>::Get())
		{
			OutLeaf.Kind = ELeafKind::Vector2D;
			return CaptureFiniteStructValue<FVector2D>(
				*Handle, OutLeaf.Value, bOutValueNotFinite);
		}
		if (StructProperty->Struct == TBaseStructure<FVector>::Get())
		{
			OutLeaf.Kind = ELeafKind::Vector;
			return CaptureFiniteStructValue<FVector>(
				*Handle, OutLeaf.Value, bOutValueNotFinite);
		}
		if (StructProperty->Struct == TBaseStructure<FVector4>::Get())
		{
			OutLeaf.Kind = ELeafKind::Vector4;
			return CaptureFiniteStructValue<FVector4>(
				*Handle, OutLeaf.Value, bOutValueNotFinite);
		}
		if (StructProperty->Struct == TBaseStructure<FQuat>::Get())
		{
			OutLeaf.Kind = ELeafKind::Quat;
			return CaptureFiniteStructValue<FQuat>(
				*Handle, OutLeaf.Value, bOutValueNotFinite);
		}
		if (StructProperty->Struct == TBaseStructure<FRotator>::Get())
		{
			OutLeaf.Kind = ELeafKind::Rotator;
			return CaptureFiniteStructValue<FRotator>(
				*Handle, OutLeaf.Value, bOutValueNotFinite);
		}
		if (StructProperty->Struct == FInstancedStruct::StaticStruct()
			&& KeyType.IsA(UBlackboardKeyType_Struct::StaticClass())
			&& Property->GetOwnerClass()
				== UBlackboardKeyType_Struct::StaticClass()
			&& Property->GetFName()
				== GET_MEMBER_NAME_CHECKED(
					UBlackboardKeyType_Struct, DefaultValue))
		{
			OutLeaf.Kind = ELeafKind::InstancedStruct;
			OutLeaf.InstancedStructValue =
				CastChecked<UBlackboardKeyType_Struct>(&KeyType)->DefaultValue;
			return true;
		}
		return false;
	}
	if (FObjectPropertyBase* ObjectProperty =
		CastField<FObjectPropertyBase>(Property))
	{
		UObject* Object = nullptr;
		if (Handle->GetValue(Object) != FPropertyAccess::Success
			|| (Object != nullptr && !IsValid(Object)))
		{
			return false;
		}
		OutLeaf.Kind = ELeafKind::Object;
		OutLeaf.ObjectClass = ObjectProperty->PropertyClass;
		OutLeaf.ObjectValue.Reset(Object);
		return true;
	}
	return false;
}

bool RestoreFiniteStructValue(
	IPropertyHandle& Handle,
	const FTypedLeaf& Leaf)
{
	auto FormatComponent = [](double Value, FString& OutFormatted)
	{
		ANSICHAR Buffer[700];
		const std::to_chars_result Formatted = std::to_chars(
			Buffer,
			Buffer + UE_ARRAY_COUNT(Buffer) - 1,
			Value,
			std::chars_format::fixed,
			340);
		if (Formatted.ec != std::errc()
			|| Formatted.ptr <= Buffer
			|| Formatted.ptr >= Buffer + UE_ARRAY_COUNT(Buffer))
		{
			return false;
		}
		*Formatted.ptr = '\0';
		OutFormatted = UTF8_TO_TCHAR(Buffer);
		return OutFormatted.Len() == Formatted.ptr - Buffer;
	};
	FString Formatted;
	switch (Leaf.Kind)
	{
	case ELeafKind::Vector2D:
	{
		const FVector2D& Value = Leaf.Value.Get<FVector2D>();
		FString X;
		FString Y;
		if (!FormatComponent(Value.X, X) || !FormatComponent(Value.Y, Y))
		{
			return false;
		}
		Formatted = TEXT("(X=") + X + TEXT(",Y=") + Y + TEXT(")");
		break;
	}
	case ELeafKind::Vector:
	{
		const FVector& Value = Leaf.Value.Get<FVector>();
		FString X;
		FString Y;
		FString Z;
		if (!FormatComponent(Value.X, X)
			|| !FormatComponent(Value.Y, Y)
			|| !FormatComponent(Value.Z, Z))
		{
			return false;
		}
		Formatted = TEXT("(X=") + X + TEXT(",Y=") + Y
			+ TEXT(",Z=") + Z + TEXT(")");
		break;
	}
	case ELeafKind::Vector4:
	{
		const FVector4& Value = Leaf.Value.Get<FVector4>();
		FString X;
		FString Y;
		FString Z;
		FString W;
		if (!FormatComponent(Value.X, X)
			|| !FormatComponent(Value.Y, Y)
			|| !FormatComponent(Value.Z, Z)
			|| !FormatComponent(Value.W, W))
		{
			return false;
		}
		Formatted = TEXT("(X=") + X + TEXT(",Y=") + Y
			+ TEXT(",Z=") + Z + TEXT(",W=") + W + TEXT(")");
		break;
	}
	case ELeafKind::Quat:
	{
		const FQuat& Value = Leaf.Value.Get<FQuat>();
		FString X;
		FString Y;
		FString Z;
		FString W;
		if (!FormatComponent(Value.X, X)
			|| !FormatComponent(Value.Y, Y)
			|| !FormatComponent(Value.Z, Z)
			|| !FormatComponent(Value.W, W))
		{
			return false;
		}
		Formatted = TEXT("(X=") + X + TEXT(",Y=") + Y
			+ TEXT(",Z=") + Z + TEXT(",W=") + W + TEXT(")");
		break;
	}
	case ELeafKind::Rotator:
	{
		const FRotator& Value = Leaf.Value.Get<FRotator>();
		FString Pitch;
		FString Yaw;
		FString Roll;
		if (!FormatComponent(Value.Pitch, Pitch)
			|| !FormatComponent(Value.Yaw, Yaw)
			|| !FormatComponent(Value.Roll, Roll))
		{
			return false;
		}
		Formatted = TEXT("(Pitch=") + Pitch + TEXT(",Yaw=") + Yaw
			+ TEXT(",Roll=") + Roll + TEXT(")");
		break;
	}
	default:
		return false;
	}
	return Handle.SetValueFromFormattedString(Formatted, RestoreFlags)
		== FPropertyAccess::Success;
}

bool RestoreLeaf(
	UBlackboardKeyType& KeyType,
	IPropertyHandle& Handle,
	const FTypedLeaf& Leaf)
{
	switch (Leaf.Kind)
	{
	case ELeafKind::Bool: return RestoreValue<bool>(Handle, Leaf.Value);
	case ELeafKind::Int8: return RestoreValue<int8>(Handle, Leaf.Value);
	case ELeafKind::Int16: return RestoreValue<int16>(Handle, Leaf.Value);
	case ELeafKind::Int32: return RestoreValue<int32>(Handle, Leaf.Value);
	case ELeafKind::Int64: return RestoreValue<int64>(Handle, Leaf.Value);
	case ELeafKind::UInt8: return RestoreValue<uint8>(Handle, Leaf.Value);
	case ELeafKind::UInt16: return RestoreValue<uint16>(Handle, Leaf.Value);
	case ELeafKind::UInt32: return RestoreValue<uint32>(Handle, Leaf.Value);
	case ELeafKind::UInt64: return RestoreValue<uint64>(Handle, Leaf.Value);
	case ELeafKind::Float: return RestoreValue<float>(Handle, Leaf.Value);
	case ELeafKind::Double: return RestoreValue<double>(Handle, Leaf.Value);
	case ELeafKind::String: return RestoreValue<FString>(Handle, Leaf.Value);
	case ELeafKind::Text: return RestoreValue<FText>(Handle, Leaf.Value);
	case ELeafKind::Name: return RestoreValue<FName>(Handle, Leaf.Value);
	case ELeafKind::Vector2D:
	case ELeafKind::Vector:
	case ELeafKind::Vector4:
	case ELeafKind::Quat:
	case ELeafKind::Rotator:
		return RestoreFiniteStructValue(Handle, Leaf);
	case ELeafKind::Object:
	{
		const UObject* Object = Leaf.ObjectValue.Get();
		return Handle.SetValue(Object, RestoreFlags) == FPropertyAccess::Success;
	}
	case ELeafKind::InstancedStruct:
	{
		UBlackboardKeyType_Struct* StructKey =
			Cast<UBlackboardKeyType_Struct>(&KeyType);
		FProperty* Property = Handle.GetProperty();
		if (StructKey == nullptr || Property == nullptr)
		{
			return false;
		}
		StructKey->PreEditChange(Property);
		StructKey->DefaultValue = Leaf.InstancedStructValue;
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Event.MemberProperty = Property;
		StructKey->PostEditChangeProperty(Event);
		return true;
	}
	}
	return false;
}

bool ReadbackLeaf(
	const UBlackboardKeyType& KeyType,
	const IPropertyHandle& Handle,
	const FTypedLeaf& Leaf)
{
	switch (Leaf.Kind)
	{
	case ELeafKind::Bool: return ReadbackValue<bool>(Handle, Leaf.Value);
	case ELeafKind::Int8: return ReadbackValue<int8>(Handle, Leaf.Value);
	case ELeafKind::Int16: return ReadbackValue<int16>(Handle, Leaf.Value);
	case ELeafKind::Int32: return ReadbackValue<int32>(Handle, Leaf.Value);
	case ELeafKind::Int64: return ReadbackValue<int64>(Handle, Leaf.Value);
	case ELeafKind::UInt8: return ReadbackValue<uint8>(Handle, Leaf.Value);
	case ELeafKind::UInt16: return ReadbackValue<uint16>(Handle, Leaf.Value);
	case ELeafKind::UInt32: return ReadbackValue<uint32>(Handle, Leaf.Value);
	case ELeafKind::UInt64: return ReadbackValue<uint64>(Handle, Leaf.Value);
	case ELeafKind::Float: return ReadbackValue<float>(Handle, Leaf.Value);
	case ELeafKind::Double: return ReadbackValue<double>(Handle, Leaf.Value);
	case ELeafKind::String: return ReadbackValue<FString>(Handle, Leaf.Value);
	case ELeafKind::Text:
	{
		FText Current;
		const FText& Captured = Leaf.Value.Get<FText>();
		return CastField<FTextProperty>(Handle.GetProperty()) != nullptr
			&& Handle.GetValue(Current) == FPropertyAccess::Success
			&& AreAuthoredTextsIdenticalForRollback(Current, Captured);
	}
	case ELeafKind::Name: return ReadbackValue<FName>(Handle, Leaf.Value);
	case ELeafKind::Vector2D: return ReadbackValue<FVector2D>(Handle, Leaf.Value);
	case ELeafKind::Vector: return ReadbackValue<FVector>(Handle, Leaf.Value);
	case ELeafKind::Vector4: return ReadbackValue<FVector4>(Handle, Leaf.Value);
	case ELeafKind::Quat: return ReadbackValue<FQuat>(Handle, Leaf.Value);
	case ELeafKind::Rotator: return ReadbackValue<FRotator>(Handle, Leaf.Value);
	case ELeafKind::Object:
	{
		UObject* Current = nullptr;
		return Handle.GetValue(Current) == FPropertyAccess::Success
			&& Current == Leaf.ObjectValue.Get();
	}
	case ELeafKind::InstancedStruct:
	{
		const UBlackboardKeyType_Struct* StructKey =
			Cast<UBlackboardKeyType_Struct>(&KeyType);
		return StructKey != nullptr
			&& StructKey->DefaultValue == Leaf.InstancedStructValue;
	}
	}
	return false;
}

struct FKeyTypeSnapshot : public FGCObject
{
	TStrongObjectPtr<UBlackboardKeyType> KeyType;
	TStrongObjectPtr<UPackage> Package;
	TSharedRef<IPropertyRowGenerator> Rows;
	TArray<FTypedLeaf> Leaves;
	FString KeyTypePath;
	FString PackageName;
	int32 KeyTypeUniqueId = 0;
	int32 PackageUniqueId = 0;
	bool bOriginalHasInstance = false;
	bool bHasEnumValidity = false;
	bool bOriginalEnumValidity = false;

	FKeyTypeSnapshot(
		UBlackboardKeyType& InKeyType,
		UPackage& InPackage,
		TSharedRef<IPropertyRowGenerator> InRows)
		: KeyType(&InKeyType)
		, Package(&InPackage)
		, Rows(MoveTemp(InRows))
		, KeyTypePath(InKeyType.GetPathName())
		, PackageName(InPackage.GetName())
		, KeyTypeUniqueId(InKeyType.GetUniqueID())
		, PackageUniqueId(InPackage.GetUniqueID())
		, bOriginalHasInstance(InKeyType.HasInstance())
	{
		if (const UBlackboardKeyType_Enum* EnumKey =
			Cast<UBlackboardKeyType_Enum>(&InKeyType))
		{
			bHasEnumValidity = true;
			bOriginalEnumValidity = EnumKey->bIsEnumNameValid != 0;
		}
	}

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		for (FTypedLeaf& Leaf : Leaves)
		{
			if (Leaf.Kind == ELeafKind::InstancedStruct)
			{
				Leaf.InstancedStructValue.AddStructReferencedObjects(Collector);
			}
		}
	}

	virtual FString GetReferencerName() const override
	{
		return TEXT("FAssetDocumentAIKeyTypeSnapshot");
	}

	bool ValidateIdentity() const
	{
		UBlackboardKeyType* Current = KeyType.Get();
		UPackage* CurrentPackage = Package.Get();
		return IsAdmittedObject(Current)
			&& CurrentPackage != nullptr
			&& Current->GetUniqueID() == KeyTypeUniqueId
			&& Current->GetPathName() == KeyTypePath
			&& CurrentPackage->GetUniqueID() == PackageUniqueId
			&& CurrentPackage->GetName() == PackageName
			&& Current->GetOutermost() == CurrentPackage
			&& DoesGeneratorSelectExactOwner(*Rows, *Current);
	}

	FAssetDocumentContractValidationResult ValidateRestorePreconditions(
		const TCHAR* DiagnosticPath) const
	{
		if (!ValidateIdentity())
		{
			return MakeValidationFailure(
				DiagnosticPath,
				TEXT("AITypedPreimage.KeyTypeRollbackIdentityMismatch"),
				TEXT("The captured Blackboard KeyType identity changed."));
		}
		UBlackboardKeyType* Current = KeyType.Get();
		check(Current != nullptr);
		for (const FTypedLeaf& Leaf : Leaves)
		{
			TSharedPtr<IPropertyHandle> Handle;
			if (!FindUniqueCurrentHandle(*Rows, *Current, Leaf, Handle))
			{
				return MakeValidationFailure(
					*Leaf.PropertyPath,
					TEXT("AITypedPreimage.KeyTypeRollbackPropertyUnavailable"),
					TEXT("A captured Blackboard KeyType property row is unavailable."));
			}
			if (!Handle->IsEditable()
				&& !ReadbackLeaf(*Current, *Handle, Leaf))
			{
				return MakeValidationFailure(
					*Leaf.PropertyPath,
					TEXT("AITypedPreimage.KeyTypeRollbackWriteFailed"),
					TEXT("A non-editable typed Blackboard KeyType property changed and cannot be restored."));
			}
		}
		return MakeValidationSuccess();
	}

	FAssetDocumentContractValidationResult RestoreAuthoredAndReadback(
		const TCHAR* DiagnosticPath)
	{
		const FAssetDocumentContractValidationResult Preconditions =
			ValidateRestorePreconditions(DiagnosticPath);
		if (!Preconditions.bValid)
		{
			return Preconditions;
		}
		UBlackboardKeyType* Current = KeyType.Get();
		check(Current != nullptr);
		for (const FTypedLeaf& Leaf : Leaves)
		{
			TSharedPtr<IPropertyHandle> Handle;
			if (!FindUniqueCurrentHandle(*Rows, *Current, Leaf, Handle))
			{
				return MakeValidationFailure(
					*Leaf.PropertyPath,
					TEXT("AITypedPreimage.KeyTypeRollbackPropertyUnavailable"),
					TEXT("A captured Blackboard KeyType property row is unavailable."));
			}
			if (!Handle->IsEditable())
			{
				// Aggregate snapshots admit authored rows disabled by a public
				// EditCondition, but never bypass that condition to write them.
				if (ReadbackLeaf(*Current, *Handle, Leaf))
				{
					continue;
				}
				return MakeValidationFailure(
					*Leaf.PropertyPath,
					TEXT("AITypedPreimage.KeyTypeRollbackWriteFailed"),
					TEXT("A non-editable typed Blackboard KeyType property changed and cannot be restored."));
			}
			if (!RestoreLeaf(*Current, *Handle, Leaf))
			{
				return MakeValidationFailure(
					*Leaf.PropertyPath,
					TEXT("AITypedPreimage.KeyTypeRollbackWriteFailed"),
					TEXT("A typed Blackboard KeyType property restore failed."));
			}
		}
		for (const FTypedLeaf& Leaf : Leaves)
		{
			TSharedPtr<IPropertyHandle> Handle;
			if (!FindUniqueCurrentHandle(*Rows, *Current, Leaf, Handle))
			{
				return MakeValidationFailure(
					*Leaf.PropertyPath,
					TEXT("AITypedPreimage.KeyTypeRollbackPropertyUnavailable"),
					TEXT("A restored Blackboard KeyType property row is unavailable."));
			}
			if (!ReadbackLeaf(*Current, *Handle, Leaf))
			{
				return MakeValidationFailure(
					*Leaf.PropertyPath,
					TEXT("AITypedPreimage.KeyTypeRollbackReadbackMismatch"),
					TEXT("A restored Blackboard KeyType property did not read back exactly."));
			}
		}
		const UBlackboardKeyType_Enum* EnumKey =
			Cast<UBlackboardKeyType_Enum>(Current);
		if (Current->HasInstance() != bOriginalHasInstance
			|| (bHasEnumValidity
				&& (EnumKey == nullptr
					|| (EnumKey->bIsEnumNameValid != 0)
						!= bOriginalEnumValidity)))
		{
			return MakeValidationFailure(
				DiagnosticPath,
				TEXT("AITypedPreimage.KeyTypeRollbackDerivedMismatch"),
				TEXT("Blackboard KeyType derived state did not restore exactly."));
		}
		return MakeValidationSuccess();
	}
};

FAssetDocumentEditorTypedPreimageResult CaptureKeyTypeSnapshot(
	const TCHAR* DiagnosticPath,
	UObject& AssetAnchor,
	UBlackboardKeyType& KeyType,
	TSharedRef<IPropertyRowGenerator> Rows,
	TConstArrayView<TSharedRef<IPropertyHandle>> Handles,
	bool bRequireEditableHandles,
	FKeyTypeSnapshot*& OutSnapshot,
	TUniquePtr<FKeyTypeSnapshot>& OutOwned)
{
	if (!IsAdmittedObject(&KeyType)
		|| KeyType.GetOutermost() == nullptr
		|| AssetAnchor.GetOutermost() != KeyType.GetOutermost())
	{
		return MakePreimageFailure(
			DiagnosticPath,
			TEXT("AITypedPreimage.KeyTypeRequired"),
			TEXT("A live same-package Blackboard KeyType is required."));
	}
	if (!DoesGeneratorSelectExactOwner(*Rows, KeyType))
	{
		return MakePreimageFailure(
			DiagnosticPath,
			TEXT("AITypedPreimage.KeyTypePropertyRowsMismatch"),
			TEXT("Property rows must select only the exact Blackboard KeyType."));
	}
	if (Handles.IsEmpty())
	{
		return MakePreimageFailure(
			DiagnosticPath,
			TEXT("AITypedPreimage.KeyTypePropertySetInvalid"),
			TEXT("The Blackboard KeyType property set must be non-empty."));
	}
	OutOwned = MakeUnique<FKeyTypeSnapshot>(
		KeyType, *KeyType.GetOutermost(), MoveTemp(Rows));
	TSet<FProperty*> UniqueProperties;
	for (const TSharedRef<IPropertyHandle>& Handle : Handles)
	{
		FProperty* Property = Handle->GetProperty();
		if (Property == nullptr || UniqueProperties.Contains(Property))
		{
			return MakePreimageFailure(
				DiagnosticPath,
				TEXT("AITypedPreimage.KeyTypePropertySetInvalid"),
				TEXT("Blackboard KeyType property rows must be unique."));
		}
		FTypedLeaf Leaf;
		bool bValueNotFinite = false;
		if (!CaptureLeaf(
			KeyType,
			OutOwned->Rows,
			Handle,
			bRequireEditableHandles,
			Leaf,
			bValueNotFinite))
		{
			return MakePreimageFailure(
				DiagnosticPath,
				bValueNotFinite
					? TEXT("AITypedPreimage.KeyTypePropertyValueNotFinite")
					: TEXT("AITypedPreimage.KeyTypePropertyNotAdmitted"),
				bValueNotFinite
					? TEXT("A Blackboard KeyType struct component is not finite.")
					: TEXT("The property is outside the closed Blackboard KeyType whitelist."));
		}
		UniqueProperties.Add(Property);
		OutOwned->Leaves.Add(MoveTemp(Leaf));
	}
	if (KeyType.IsA(UBlackboardKeyType_Enum::StaticClass()))
	{
		const FName EnumName =
			GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Enum, EnumName);
		const FName EnumType =
			GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Enum, EnumType);
		OutOwned->Leaves.StableSort(
			[EnumName, EnumType](const FTypedLeaf& Left, const FTypedLeaf& Right)
			{
				auto Priority = [EnumName, EnumType](FName Name)
				{
					return Name == EnumName ? 0 : (Name == EnumType ? 1 : 2);
				};
				return Priority(Left.PropertyName)
					< Priority(Right.PropertyName);
			});
	}
	OutSnapshot = OutOwned.Get();
	return MakePreimageSuccess();
}

class FKeyTypeRollbackState final : public FAliasRollbackState
{
public:
	FKeyTypeRollbackState(
		UObject& InAsset,
		uint64 InAssetTypeRevision,
		TUniquePtr<FKeyTypeSnapshot> InSnapshot,
		bool bInPackageDirty)
		: Asset(&InAsset)
		, AssetPath(InAsset.GetPathName())
		, AssetUniqueId(InAsset.GetUniqueID())
		, AssetTypeRevision(InAssetTypeRevision)
		, Snapshot(MoveTemp(InSnapshot))
		, bPackageDirty(bInPackageDirty)
	{
	}

protected:
	virtual FAssetDocumentContractValidationResult RestoreOnce() override
	{
		if (!IsEditorGameThread())
		{
			return MakeValidationFailure(
				KeyTypePath,
				TEXT("AITypedPreimage.InvalidExecutionContext"),
				TEXT("Blackboard KeyType rollback requires the Editor game thread."));
		}
		UObject* CurrentAsset = Asset.Get();
		if (!IsAdmittedObject(CurrentAsset)
			|| CurrentAsset->GetPathName() != AssetPath
			|| CurrentAsset->GetUniqueID() != AssetUniqueId
			|| static_cast<uint64>(CurrentAsset->GetUniqueID())
				!= AssetTypeRevision
			|| !Snapshot->ValidateIdentity())
		{
			return MakeValidationFailure(
				KeyTypePath,
				TEXT("AITypedPreimage.KeyTypeRollbackIdentityMismatch"),
				TEXT("The Asset or Blackboard KeyType identity changed."));
		}
		const FAssetDocumentContractValidationResult Restored =
			Snapshot->RestoreAuthoredAndReadback(KeyTypePath);
		if (!Restored.bValid)
		{
			return Restored;
		}
		UPackage* Package = Snapshot->Package.Get();
		check(Package != nullptr);
		Package->SetDirtyFlag(bPackageDirty);
		if (Package->IsDirty() != bPackageDirty)
		{
			return MakeValidationFailure(
				KeyTypePath,
				TEXT("AITypedPreimage.KeyTypeRollbackReadbackMismatch"),
				TEXT("The Blackboard KeyType package dirty preimage did not restore."));
		}
		return MakeValidationSuccess();
	}

private:
	TStrongObjectPtr<UObject> Asset;
	FString AssetPath;
	int32 AssetUniqueId = 0;
	uint64 AssetTypeRevision = 0;
	TUniquePtr<FKeyTypeSnapshot> Snapshot;
	bool bPackageDirty = false;
};

void GatherTopLevelAuthoredHandles(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	UObject& Owner,
	TArray<TSharedRef<IPropertyHandle>>& OutHandles)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		FProperty* Property = Handle.IsValid() ? Handle->GetProperty() : nullptr;
		if (Handle.IsValid()
			&& Property != nullptr
			&& Property->GetOwnerProperty() == Property
			&& Handle->GeneratePathToProperty() == Property->GetName()
			&& IsAuthoredPropertyHandle(*Handle, *Property)
			&& HasExactOuterObject(*Handle, Owner)
			&& !OutHandles.ContainsByPredicate(
				[Property](const TSharedRef<IPropertyHandle>& Existing)
				{
					return Existing->GetProperty() == Property;
				}))
		{
			OutHandles.Add(Handle.ToSharedRef());
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		GatherTopLevelAuthoredHandles(Children, Owner, OutHandles);
	}
}

int32 CountExpectedTopLevelAuthoredProperties(const UBlackboardKeyType& KeyType)
{
	int32 Count = 0;
	for (TFieldIterator<FProperty> It(
		KeyType.GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		const FProperty& Property = **It;
		if (Property.GetOwnerProperty() == &Property
			&& Property.HasAnyPropertyFlags(CPF_Edit)
			&& !Property.HasAnyPropertyFlags(CPF_Transient | CPF_EditConst))
		{
			++Count;
		}
	}
	return Count;
}

class FCompleteKeyTypeRows final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FCompleteKeyTypeRows>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
	{
		(void)DetailBuilder;
	}
};

TSharedRef<IPropertyRowGenerator> MakeRows(UObject& Object)
{
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = true;
	Args.bShouldShowHiddenProperties = true;
	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(
			TEXT("PropertyEditor"));
	TSharedRef<IPropertyRowGenerator> Rows =
		PropertyEditor.CreatePropertyRowGenerator(Args);
	Rows->OnRowsRefreshed().AddLambda([]() {});
	Rows->RegisterInstancedCustomPropertyLayout(
		Object.GetClass(),
		FOnGetDetailCustomizationInstance::CreateStatic(
			&FCompleteKeyTypeRows::MakeInstance));
	Rows->SetObjects({&Object});
	return Rows;
}

bool BlackboardEntriesEqualExact(
	TConstArrayView<FBlackboardEntry> Left,
	TConstArrayView<FBlackboardEntry> Right);

struct FProjectionSnapshot
{
	TStrongObjectPtr<UBlackboardData> Blackboard;
	FString Path;
	int32 UniqueId = 0;
	TStrongObjectPtr<UPackage> Package;
	FString PackageName;
	int32 PackageUniqueId = 0;
	TStrongObjectPtr<UBlackboardData> Parent;
	TArray<FBlackboardEntry> ParentKeys;
	TArray<FName> KeyNames;
	TArray<TWeakObjectPtr<UClass>> KeyTypes;
	TArray<bool> InstanceSynced;
	FBlackboard::FKey FirstKeyId = FBlackboard::InvalidKey;
	bool bHasSynchronizedKeys = false;
	bool bIsRoot = false;

	FProjectionSnapshot(UBlackboardData& InBlackboard, bool bInIsRoot)
		: Blackboard(&InBlackboard)
		, Path(InBlackboard.GetPathName())
		, UniqueId(InBlackboard.GetUniqueID())
		, Package(InBlackboard.GetOutermost())
		, PackageName(InBlackboard.GetOutermost()->GetName())
		, PackageUniqueId(InBlackboard.GetOutermost()->GetUniqueID())
		, Parent(InBlackboard.Parent)
		, ParentKeys(InBlackboard.ParentKeys)
		, FirstKeyId(InBlackboard.GetFirstKeyID())
		, bHasSynchronizedKeys(InBlackboard.HasSynchronizedKeys())
		, bIsRoot(bInIsRoot)
	{
		for (int32 Index = 0; Index < InBlackboard.GetNumKeys(); ++Index)
		{
			const FBlackboard::FKey KeyId(Index);
			KeyNames.Add(InBlackboard.GetKeyName(KeyId));
			KeyTypes.Add(InBlackboard.GetKeyType(KeyId).Get());
			InstanceSynced.Add(InBlackboard.IsKeyInstanceSynced(KeyId));
		}
	}

	bool ValidateIdentity(const UBlackboardData& Root) const
	{
		UBlackboardData* Current = Blackboard.Get();
		UPackage* CurrentPackage = Package.Get();
		return IsAdmittedObject(Current)
			&& (bIsRoot
				? Current == &Root
				: (Current != &Root && Current->IsChildOf(Root)))
			&& Current->Parent == Parent.Get()
			&& Current->GetPathName() == Path
			&& Current->GetUniqueID() == UniqueId
			&& CurrentPackage != nullptr
			&& CurrentPackage->GetName() == PackageName
			&& CurrentPackage->GetUniqueID() == PackageUniqueId
			&& Current->GetOutermost() == CurrentPackage;
	}

	bool MatchesProjection() const
	{
		const UBlackboardData* Current = Blackboard.Get();
		if (Current == nullptr
			|| !BlackboardEntriesEqualExact(Current->ParentKeys, ParentKeys)
			|| Current->GetFirstKeyID() != FirstKeyId
			|| Current->HasSynchronizedKeys() != bHasSynchronizedKeys
			|| Current->GetNumKeys() != KeyNames.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < KeyNames.Num(); ++Index)
		{
			const FBlackboard::FKey KeyId(Index);
			if (Current->GetKeyName(KeyId) != KeyNames[Index]
				|| Current->GetKeyID(KeyNames[Index]) != KeyId
				|| Current->GetKeyType(KeyId).Get() != KeyTypes[Index].Get()
				|| Current->IsKeyInstanceSynced(KeyId)
					!= InstanceSynced[Index])
			{
				return false;
			}
		}
		return true;
	}
};

bool BlackboardEntryEqualsExact(
	const FBlackboardEntry& Left,
	const FBlackboardEntry& Right)
{
	if (Left.EntryName != Right.EntryName
		|| Left.KeyType.Get() != Right.KeyType.Get()
		|| (Left.KeyType != nullptr && Right.KeyType != nullptr
			&& Left.KeyType->GetClass() != Right.KeyType->GetClass())
		|| (Left.bInstanceSynced != 0) != (Right.bInstanceSynced != 0))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	return Left.EntryDescription == Right.EntryDescription
		&& Left.EntryCategory == Right.EntryCategory;
#else
	return true;
#endif
}

bool BlackboardEntriesEqualExact(
	TConstArrayView<FBlackboardEntry> Left,
	TConstArrayView<FBlackboardEntry> Right)
{
	if (Left.Num() != Right.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Num(); ++Index)
	{
		if (!BlackboardEntryEqualsExact(Left[Index], Right[Index]))
		{
			return false;
		}
	}
	return true;
}

struct FDirtySnapshot
{
	TStrongObjectPtr<UPackage> Package;
	FString Name;
	int32 UniqueId = 0;
	bool bDirty = false;
};

class FLocalKeysRollbackState final : public FAliasRollbackState
{
public:
	FLocalKeysRollbackState(
		UBlackboardData& InRoot,
		uint64 InAssetTypeRevision,
		TArray<FBlackboardEntry> InKeys,
		TArray<TStrongObjectPtr<UBlackboardKeyType>> InRetainedLocalKeyTypes,
		TArray<TUniquePtr<FKeyTypeSnapshot>> InKeyTypes,
		TArray<FProjectionSnapshot> InProjections,
		TArray<FDirtySnapshot> InDirty,
		TArray<TStrongObjectPtr<UBlackboardKeyType>> InRetainedProjectionTypes)
		: Root(&InRoot)
		, RootPath(InRoot.GetPathName())
		, RootUniqueId(InRoot.GetUniqueID())
		, AssetTypeRevision(InAssetTypeRevision)
		, Keys(MoveTemp(InKeys))
		, RetainedLocalKeyTypes(MoveTemp(InRetainedLocalKeyTypes))
		, KeyTypes(MoveTemp(InKeyTypes))
		, Projections(MoveTemp(InProjections))
		, Dirty(MoveTemp(InDirty))
		, RetainedProjectionTypes(MoveTemp(InRetainedProjectionTypes))
	{
	}

protected:
	virtual FAssetDocumentContractValidationResult RestoreOnce() override
	{
		if (!IsEditorGameThread())
		{
			return MakeValidationFailure(
				LocalKeysPath,
				TEXT("AITypedPreimage.InvalidExecutionContext"),
				TEXT("Blackboard local-keys rollback requires the Editor game thread."));
		}
		UBlackboardData* CurrentRoot = Root.Get();
		if (!IsAdmittedObject(CurrentRoot)
			|| CurrentRoot->GetPathName() != RootPath
			|| CurrentRoot->GetUniqueID() != RootUniqueId
			|| static_cast<uint64>(CurrentRoot->GetUniqueID())
				!= AssetTypeRevision)
		{
			return MakeValidationFailure(
				LocalKeysPath,
				TEXT("AITypedPreimage.BlackboardLocalKeysRollbackIdentityMismatch"),
				TEXT("The exact Blackboard Asset identity changed."));
		}
		for (const TUniquePtr<FKeyTypeSnapshot>& Snapshot : KeyTypes)
		{
			if (!Snapshot->ValidateIdentity())
			{
				return MakeValidationFailure(
					LocalKeysPath,
					TEXT("AITypedPreimage.BlackboardLocalKeysRollbackIdentityMismatch"),
					TEXT("A captured local KeyType identity changed."));
			}
		}
		for (const FProjectionSnapshot& Projection : Projections)
		{
			if (!Projection.ValidateIdentity(*CurrentRoot))
			{
				return MakeValidationFailure(
					LocalKeysPath,
					TEXT("AITypedPreimage.BlackboardLocalKeysRollbackIdentityMismatch"),
					TEXT("A loaded descendant Blackboard identity changed."));
			}
		}
		TArray<UBlackboardData*> CurrentDescendants;
		for (TObjectIterator<UBlackboardData> It; It; ++It)
		{
			if (*It != CurrentRoot
				&& IsAdmittedObject(*It)
				&& It->IsChildOf(*CurrentRoot))
			{
				CurrentDescendants.Add(*It);
			}
		}
		CurrentDescendants.Sort(
			[](const UBlackboardData& Left, const UBlackboardData& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});
		if (Projections.Num() != CurrentDescendants.Num() + 1)
		{
			return MakeValidationFailure(
				LocalKeysPath,
				TEXT("AITypedPreimage.BlackboardLocalKeysRollbackIdentityMismatch"),
				TEXT("The loaded descendant Blackboard set changed."));
		}
		for (int32 Index = 0; Index < CurrentDescendants.Num(); ++Index)
		{
			if (Projections[Index + 1].Blackboard.Get()
				!= CurrentDescendants[Index])
			{
				return MakeValidationFailure(
					LocalKeysPath,
					TEXT("AITypedPreimage.BlackboardLocalKeysRollbackIdentityMismatch"),
					TEXT("The loaded descendant Blackboard identity set changed."));
			}
		}
		for (const FDirtySnapshot& Item : Dirty)
		{
			if (Item.Package.Get() == nullptr
				|| Item.Package->GetName() != Item.Name
				|| Item.Package->GetUniqueID() != Item.UniqueId)
			{
				return MakeValidationFailure(
					LocalKeysPath,
					TEXT("AITypedPreimage.BlackboardLocalKeysRollbackIdentityMismatch"),
					TEXT("A captured Blackboard package identity changed."));
			}
		}
		for (const TUniquePtr<FKeyTypeSnapshot>& Snapshot : KeyTypes)
		{
			const FAssetDocumentContractValidationResult Preconditions =
				Snapshot->ValidateRestorePreconditions(LocalKeysPath);
			if (!Preconditions.bValid)
			{
				return Preconditions;
			}
		}

		FArrayProperty* KeysProperty = CastField<FArrayProperty>(
			UBlackboardData::StaticClass()->FindPropertyByName(
				GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys)));
		if (KeysProperty == nullptr)
		{
			return MakeValidationFailure(
				LocalKeysPath,
				TEXT("AITypedPreimage.BlackboardLocalKeysRollbackWriteFailed"),
				TEXT("The public UBlackboardData::Keys metadata is unavailable."));
		}
		CurrentRoot->PreEditChange(KeysProperty);
		CurrentRoot->Keys = Keys;
		FPropertyChangedEvent Event(
			KeysProperty,
			EPropertyChangeType::ArrayClear | EPropertyChangeType::ValueSet);
		Event.MemberProperty = KeysProperty;
		CurrentRoot->PostEditChangeProperty(Event);

		for (const TUniquePtr<FKeyTypeSnapshot>& Snapshot : KeyTypes)
		{
			const FAssetDocumentContractValidationResult Restored =
				Snapshot->RestoreAuthoredAndReadback(LocalKeysPath);
			if (!Restored.bValid)
			{
				return Restored;
			}
		}
		CurrentRoot->UpdateParentKeys();
		CurrentRoot->UpdateKeyIDs();
		CurrentRoot->UpdateIfHasSynchronizedKeys();
		CurrentRoot->PropagateKeyChangesToDerivedBlackboardAssets();

		if (!BlackboardEntriesEqualExact(CurrentRoot->Keys, Keys))
		{
			return MakeValidationFailure(
				LocalKeysPath,
				TEXT("AITypedPreimage.BlackboardLocalKeysRollbackReadbackMismatch"),
				TEXT("The ordered local Blackboard entries did not restore exactly."));
		}
		for (const FProjectionSnapshot& Projection : Projections)
		{
			if (!Projection.MatchesProjection())
			{
				return MakeValidationFailure(
					LocalKeysPath,
					TEXT("AITypedPreimage.BlackboardLocalKeysRollbackDerivedMismatch"),
					TEXT("A loaded descendant Blackboard projection did not restore."));
			}
		}
		for (const FDirtySnapshot& Item : Dirty)
		{
			Item.Package->SetDirtyFlag(Item.bDirty);
			if (Item.Package->IsDirty() != Item.bDirty)
			{
				return MakeValidationFailure(
					LocalKeysPath,
					TEXT("AITypedPreimage.BlackboardLocalKeysRollbackDirtyMismatch"),
					TEXT("A Blackboard package dirty preimage did not restore."));
			}
		}
		return MakeValidationSuccess();
	}

private:
	TStrongObjectPtr<UBlackboardData> Root;
	FString RootPath;
	int32 RootUniqueId = 0;
	uint64 AssetTypeRevision = 0;
	TArray<FBlackboardEntry> Keys;
	TArray<TStrongObjectPtr<UBlackboardKeyType>> RetainedLocalKeyTypes;
	TArray<TUniquePtr<FKeyTypeSnapshot>> KeyTypes;
	TArray<FProjectionSnapshot> Projections;
	TArray<FDirtySnapshot> Dirty;
	TArray<TStrongObjectPtr<UBlackboardKeyType>> RetainedProjectionTypes;
};

void AddDirtySnapshot(
	UPackage& Package,
	UPackage& RootPackage,
	TArray<FDirtySnapshot>& OutDirty)
{
	if (OutDirty.ContainsByPredicate(
		[&Package](const FDirtySnapshot& Item)
		{
			return Item.Package.Get() == &Package;
		}))
	{
		return;
	}
	FDirtySnapshot Item;
	Item.Package.Reset(&Package);
	Item.Name = Package.GetName();
	Item.UniqueId = Package.GetUniqueID();
	Item.bDirty = Package.IsDirty();
	if (&Package == &RootPackage)
	{
		OutDirty.Add(MoveTemp(Item));
	}
	else
	{
		OutDirty.Insert(MoveTemp(Item), 0);
	}
}

} // namespace UE::AssetDocument::AI::TypedPreimage::Blackboard

FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
	const IAssetDocumentVerifiedHandleLease& AssetLease,
	UObject& ExactKeyType,
	TSharedRef<IPropertyRowGenerator> PropertyRows,
	TConstArrayView<TSharedRef<IPropertyHandle>> PropertyHandles,
	TConstArrayView<FString> RollbackComponentIds,
	IAssetDocumentAtomicTransaction& Transaction)
{
	using namespace UE::AssetDocument::AI::TypedPreimage::Blackboard;
	FAssetDocumentEditorTypedPreimageResult Result;
	const FAssetDocumentContractValidationResult IdentityResult =
		WithAssetDocumentEditorAssetIdentity(
			AssetLease,
			[&](const FAssetDocumentEditorAssetIdentityView& Identity)
			{
				Result = ValidateCommonCapture(
					KeyTypePath,
					AssetLease,
					Identity,
					Transaction,
					RollbackComponentIds);
				if (!Result.bSucceeded)
				{
					FAssetDocumentContractValidationResult Failure;
					Failure.Diagnostics = Result.Diagnostics;
					return Failure;
				}
				UBlackboardKeyType* KeyType =
					Cast<UBlackboardKeyType>(&ExactKeyType);
				if (KeyType == nullptr)
				{
					Result = MakePreimageFailure(
						KeyTypePath,
						TEXT("AITypedPreimage.KeyTypeRequired"),
						TEXT("The exact object must be a Blackboard KeyType."));
					FAssetDocumentContractValidationResult Failure;
					Failure.Diagnostics = Result.Diagnostics;
					return Failure;
				}
				TUniquePtr<FKeyTypeSnapshot> Snapshot;
				FKeyTypeSnapshot* SnapshotView = nullptr;
				Result = CaptureKeyTypeSnapshot(
					KeyTypePath,
					Identity.GetObject(),
					*KeyType,
					MoveTemp(PropertyRows),
					PropertyHandles,
					true,
					SnapshotView,
					Snapshot);
				(void)SnapshotView;
				if (!Result.bSucceeded)
				{
					FAssetDocumentContractValidationResult Failure;
					Failure.Diagnostics = Result.Diagnostics;
					return Failure;
				}
				const TSharedRef<FKeyTypeRollbackState> State =
					MakeShared<FKeyTypeRollbackState>(
						Identity.GetObject(),
						Identity.GetTypeRevision(),
						MoveTemp(Snapshot),
						KeyType->GetOutermost()->IsDirty());
				Result = EnlistAliasActions(
					KeyTypePath, RollbackComponentIds, State, Transaction);
				if (Result.bSucceeded)
				{
					return MakeValidationSuccess();
				}
				FAssetDocumentContractValidationResult Failure;
				Failure.Diagnostics = Result.Diagnostics;
				return Failure;
			});
	if (!IdentityResult.bValid && Result.Diagnostics.IsEmpty())
	{
		Result = IdentityFailure(IdentityResult);
	}
	return Result;
}

FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
	const IAssetDocumentVerifiedHandleLease& AssetLease,
	UObject& ExactBlackboardData,
	TConstArrayView<FString> RollbackComponentIds,
	IAssetDocumentAtomicTransaction& Transaction)
{
	using namespace UE::AssetDocument::AI::TypedPreimage::Blackboard;
	FAssetDocumentEditorTypedPreimageResult Result;
	const FAssetDocumentContractValidationResult IdentityResult =
		WithAssetDocumentEditorAssetIdentity(
			AssetLease,
			[&](const FAssetDocumentEditorAssetIdentityView& Identity)
			{
				Result = ValidateCommonCapture(
					LocalKeysPath,
					AssetLease,
					Identity,
					Transaction,
					RollbackComponentIds);
				if (!Result.bSucceeded)
				{
					FAssetDocumentContractValidationResult Failure;
					Failure.Diagnostics = Result.Diagnostics;
					return Failure;
				}
				UBlackboardData* Root = Cast<UBlackboardData>(&ExactBlackboardData);
				if (Root == nullptr
					|| Root != &Identity.GetObject()
					|| !IsAdmittedObject(Root))
				{
					Result = MakePreimageFailure(
						LocalKeysPath,
						TEXT("AITypedPreimage.BlackboardDataRequired"),
						TEXT("The exact Asset identity must be a live UBlackboardData."));
					FAssetDocumentContractValidationResult Failure;
					Failure.Diagnostics = Result.Diagnostics;
					return Failure;
				}

					TArray<TUniquePtr<FKeyTypeSnapshot>> KeyTypeSnapshots;
					TArray<TStrongObjectPtr<UBlackboardKeyType>>
						RetainedLocalKeyTypes;
					TSet<UBlackboardKeyType*> UniqueKeyTypes;
				for (const FBlackboardEntry& Entry : Root->Keys)
				{
					UBlackboardKeyType* KeyType = Entry.KeyType.Get();
					if (!IsAdmittedObject(KeyType)
						|| KeyType->GetOutermost() != Root->GetOutermost())
					{
						Result = MakePreimageFailure(
							LocalKeysPath,
							TEXT("AITypedPreimage.BlackboardLocalKeysInvalid"),
							TEXT("Every local Blackboard entry requires a live same-package KeyType."));
						FAssetDocumentContractValidationResult Failure;
						Failure.Diagnostics = Result.Diagnostics;
						return Failure;
					}
					if (UniqueKeyTypes.Contains(KeyType))
					{
						continue;
						}
						UniqueKeyTypes.Add(KeyType);
						RetainedLocalKeyTypes.Emplace(KeyType);
						TSharedRef<IPropertyRowGenerator> Rows = MakeRows(*KeyType);
					TArray<TSharedRef<IPropertyHandle>> Handles;
					GatherTopLevelAuthoredHandles(
						Rows->GetRootTreeNodes(), *KeyType, Handles);
					const int32 ExpectedPropertyCount =
						CountExpectedTopLevelAuthoredProperties(*KeyType);
					if (Handles.Num() != ExpectedPropertyCount)
					{
						FString Message =
							TEXT("The local KeyType authored row closure is incomplete (rows=");
						Message += FString::FromInt(Handles.Num());
						Message += TEXT(", expected=");
						Message += FString::FromInt(ExpectedPropertyCount);
						Message += TEXT(", rebound=");
						for (int32 HandleIndex = 0;
							HandleIndex < Handles.Num(); ++HandleIndex)
						{
							if (HandleIndex > 0)
							{
								Message += TEXT("|");
							}
							Message += Handles[HandleIndex]->GetProperty()->GetName();
						}
						Message += TEXT(").");
						Result = MakePreimageFailure(
							LocalKeysPath,
							TEXT("AITypedPreimage.BlackboardLocalKeysInvalid"),
							*Message);
						FAssetDocumentContractValidationResult Failure;
						Failure.Diagnostics = Result.Diagnostics;
						return Failure;
					}
					if (Handles.IsEmpty())
					{
						continue;
					}
					TUniquePtr<FKeyTypeSnapshot> Snapshot;
					FKeyTypeSnapshot* SnapshotView = nullptr;
					Result = CaptureKeyTypeSnapshot(
						LocalKeysPath,
						Identity.GetObject(),
						*KeyType,
						Rows,
						Handles,
						false,
						SnapshotView,
						Snapshot);
					(void)SnapshotView;
					if (!Result.bSucceeded)
					{
						Result.Diagnostics[0].Code =
							TEXT("AITypedPreimage.BlackboardLocalKeysInvalid");
						FAssetDocumentContractValidationResult Failure;
						Failure.Diagnostics = Result.Diagnostics;
						return Failure;
					}
					KeyTypeSnapshots.Add(MoveTemp(Snapshot));
				}

				TArray<UBlackboardData*> Descendants;
				for (TObjectIterator<UBlackboardData> It; It; ++It)
				{
					if (*It != Root
						&& IsAdmittedObject(*It)
						&& It->IsChildOf(*Root))
					{
						Descendants.Add(*It);
					}
				}
				Descendants.Sort(
					[](const UBlackboardData& Left, const UBlackboardData& Right)
					{
						return Left.GetPathName() < Right.GetPathName();
					});
				TArray<FProjectionSnapshot> Projections;
				Projections.Emplace(*Root, true);
				TArray<TStrongObjectPtr<UBlackboardKeyType>> RetainedProjectionTypes;
				TSet<UBlackboardKeyType*> UniqueProjectionTypes;
				for (UBlackboardData* Descendant : Descendants)
				{
					Projections.Emplace(*Descendant, false);
					for (int32 Index = 0; Index < Descendant->GetNumKeys(); ++Index)
					{
						const FBlackboardEntry* Entry =
							Descendant->GetKey(FBlackboard::FKey(Index));
						UBlackboardKeyType* KeyType =
							Entry != nullptr ? Entry->KeyType.Get() : nullptr;
						if (KeyType != nullptr
							&& !UniqueProjectionTypes.Contains(KeyType))
						{
							UniqueProjectionTypes.Add(KeyType);
							RetainedProjectionTypes.Emplace(KeyType);
						}
					}
				}

				TArray<FDirtySnapshot> Dirty;
				UPackage* RootPackage = Root->GetOutermost();
				check(RootPackage != nullptr);
				for (UBlackboardData* Descendant : Descendants)
				{
					AddDirtySnapshot(
						*Descendant->GetOutermost(), *RootPackage, Dirty);
				}
				AddDirtySnapshot(*RootPackage, *RootPackage, Dirty);

				const TSharedRef<FLocalKeysRollbackState> State =
					MakeShared<FLocalKeysRollbackState>(
							*Root,
							Identity.GetTypeRevision(),
							Root->Keys,
							MoveTemp(RetainedLocalKeyTypes),
							MoveTemp(KeyTypeSnapshots),
						MoveTemp(Projections),
						MoveTemp(Dirty),
						MoveTemp(RetainedProjectionTypes));
				Result = EnlistAliasActions(
					LocalKeysPath, RollbackComponentIds, State, Transaction);
				if (Result.bSucceeded)
				{
					return MakeValidationSuccess();
				}
				FAssetDocumentContractValidationResult Failure;
				Failure.Diagnostics = Result.Diagnostics;
				return Failure;
			});
	if (!IdentityResult.bValid && Result.Diagnostics.IsEmpty())
	{
		Result = IdentityFailure(IdentityResult);
	}
	return Result;
}
