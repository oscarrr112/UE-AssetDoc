// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalRead.h"

#include "Provider/AssetDocumentAIProviderContracts.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "Editor.h"
#include "Internationalization/Text.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/InstancedStruct.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/EnumProperty.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalRead
{
namespace AIBlackboardKeyTypeStructCanonicalReadLocal
{
constexpr int32 MaximumDepth = 16;
constexpr int32 MaximumFields = 256;
constexpr int32 MaximumContainerEntries = 1024;
constexpr int32 MaximumNodes = 4096;
constexpr int64 MaximumComparisonBytes = 1024 * 1024;
constexpr int32 MaximumStringBytes = 256 * 1024;

struct FContext
{
	Private::FReadBudgetPrecharge Precharge = Private::GetReadBudgetPrecharge();
	int32 RemainingFields = MaximumFields - Precharge.Fields;
	int32 RemainingContainerEntries = MaximumContainerEntries - Precharge.ContainerEntries;
	int32 RemainingNodes = MaximumNodes - Precharge.Nodes;
	int64 RemainingComparisonBytes = MaximumComparisonBytes - Precharge.ComparisonBytes;
	EFailure Failure = EFailure::None;
	TArray<TPair<const UScriptStruct*, const void*>> ActiveStructs;
};

FAssetDocumentRuntimeValueDto Scalar(const EAssetDocumentRuntimeValueKind Kind)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = Kind;
	return Result;
}

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result = Scalar(EAssetDocumentRuntimeValueKind::String);
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result = Scalar(EAssetDocumentRuntimeValueKind::Object);
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

bool Fail(FContext& Context, const EFailure Failure)
{
	if (Context.Failure == EFailure::None)
	{
		Context.Failure = Failure;
	}
	return false;
}

bool ConsumeNode(FContext& Context, const int32 Count = 1)
{
	if (Count < 0 || Context.RemainingNodes < Count)
	{
		return Fail(Context, EFailure::BudgetExceeded);
	}
	Context.RemainingNodes -= Count;
	return true;
}

bool ConsumeContainer(FContext& Context, const int32 Count)
{
	if (Count < 0 || Count > MaximumContainerEntries
		|| Context.RemainingContainerEntries < Count)
	{
		return Fail(Context, EFailure::BudgetExceeded);
	}
	Context.RemainingContainerEntries -= Count;
	return true;
}

bool CheckString(FContext& Context, const FString& Value)
{
	const FTCHARToUTF8 Utf8(*Value, Value.Len());
	if (Utf8.Length() > MaximumStringBytes) return Fail(Context, EFailure::BudgetExceeded);
	if (!IsAssetDocumentAICanonicalRuntimeValue(StringValue(Value)))
		return Fail(Context, EFailure::DtoInvalid);
	return true;
}

bool AppendUint64(TArray<uint8>& Bytes, const uint64 Value, const int64 Limit)
{
	if (Limit < 8 || Bytes.Num() > Limit - 8) return false;
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Bytes.Add(static_cast<uint8>(Value >> Shift));
	}
	return true;
}

bool EncodeCanonical(const FAssetDocumentRuntimeValueDto& Value, TArray<uint8>& Bytes, const int64 Limit)
{
	if (Bytes.Num() >= Limit) return false;
	Bytes.Add(static_cast<uint8>(Value.Kind));
	switch (Value.Kind)
	{
	case EAssetDocumentRuntimeValueKind::Null: return true;
	case EAssetDocumentRuntimeValueKind::Boolean:
		if (Bytes.Num() >= Limit) return false;
		Bytes.Add(Value.BooleanValue ? 1 : 0); return true;
	case EAssetDocumentRuntimeValueKind::Integer:
		return AppendUint64(Bytes, static_cast<uint64>(Value.IntegerValue), Limit);
	case EAssetDocumentRuntimeValueKind::Number:
		return AppendUint64(Bytes, BitCast<uint64>(Value.NumberValue == 0.0 ? 0.0 : Value.NumberValue), Limit);
	case EAssetDocumentRuntimeValueKind::String:
	{
		const FTCHARToUTF8 Utf8(*Value.StringValue, Value.StringValue.Len());
		if (!AppendUint64(Bytes, static_cast<uint64>(Utf8.Length()), Limit)
			|| Utf8.Length() > Limit - Bytes.Num()) return false;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return true;
	}
	case EAssetDocumentRuntimeValueKind::Array:
		if (!AppendUint64(Bytes, static_cast<uint64>(Value.Elements.Num()), Limit)) return false;
		for (const FAssetDocumentRuntimeValueDto& Child : Value.Elements)
		{
			if (!EncodeCanonical(Child, Bytes, Limit)) return false;
		}
		return true;
	case EAssetDocumentRuntimeValueKind::Object:
		if (Value.ObjectKeys.Num() != Value.Elements.Num()) return false;
		if (!AppendUint64(Bytes, static_cast<uint64>(Value.Elements.Num()), Limit)) return false;
		for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
		{
			FAssetDocumentRuntimeValueDto Key = StringValue(Value.ObjectKeys[Index]);
			if (!EncodeCanonical(Key, Bytes, Limit)
				|| !EncodeCanonical(Value.Elements[Index], Bytes, Limit)) return false;
		}
		return true;
	default: return false;
	}
}

bool MakeSortKey(FContext& Context, const FAssetDocumentRuntimeValueDto& Value, TArray<uint8>& Out)
{
	if (!EncodeCanonical(Value, Out, Context.RemainingComparisonBytes))
	{
		return Fail(Context, EFailure::BudgetExceeded);
	}
	Context.RemainingComparisonBytes -= Out.Num();
	return true;
}

bool BytesLess(const TArray<uint8>& Left, const TArray<uint8>& Right)
{
	const int32 Common = FMath::Min(Left.Num(), Right.Num());
	const int32 Comparison = Common == 0 ? 0 : FMemory::Memcmp(Left.GetData(), Right.GetData(), Common);
	return Comparison == 0 ? Left.Num() < Right.Num() : Comparison < 0;
}

bool ReadProperty(const FProperty&, const void*, int32, FContext&, FAssetDocumentRuntimeValueDto&);

bool ReadStruct(
	const UScriptStruct& ScriptStruct,
	const void* Memory,
	const int32 Depth,
	FContext& Context,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	if (Memory == nullptr) return Fail(Context, EFailure::InvalidContext);
	if (Depth > MaximumDepth) return Fail(Context, EFailure::BudgetExceeded);
	const TPair<const UScriptStruct*, const void*> Identity(&ScriptStruct, Memory);
	if (Context.ActiveStructs.Contains(Identity)) return Fail(Context, EFailure::SchemaDrift);
	if (!ConsumeNode(Context, 3)) return false;
	Context.ActiveStructs.Add(Identity);
	ON_SCOPE_EXIT { Context.ActiveStructs.Pop(); };
	TArray<TPair<FString, const FProperty*>> Fields;
	EFailure FieldFailure = EFailure::None;
	if (!Private::CollectStoredFields(
		ScriptStruct, Context.RemainingFields, Fields, FieldFailure))
		return Fail(Context, FieldFailure);
	TArray<FString> Keys;
	TArray<FAssetDocumentRuntimeValueDto> Values;
	Keys.Reserve(Fields.Num()); Values.Reserve(Fields.Num());
	for (const TPair<FString, const FProperty*>& Field : Fields)
	{
		if ((!Keys.IsEmpty() && Keys.Last() == Field.Key) || !CheckString(Context, Field.Key))
		{
			return Fail(Context, EFailure::SchemaDrift);
		}
		FAssetDocumentRuntimeValueDto Value;
		if (Field.Value->ArrayDim > 1)
		{
			if (!ConsumeNode(Context) || !ConsumeContainer(Context, Field.Value->ArrayDim)) return false;
			Value.Kind = EAssetDocumentRuntimeValueKind::Array;
			Value.Elements.Reserve(Field.Value->ArrayDim);
			for (int32 Index = 0; Index < Field.Value->ArrayDim; ++Index)
			{
				FAssetDocumentRuntimeValueDto Element;
				if (!ReadProperty(*Field.Value,
					Field.Value->ContainerPtrToValuePtr<void>(Memory, Index), Depth + 1, Context, Element)) return false;
				Value.Elements.Add(MoveTemp(Element));
			}
		}
		else if (!ReadProperty(*Field.Value,
			Field.Value->ContainerPtrToValuePtr<void>(Memory), Depth + 1, Context, Value)) return false;
		Keys.Add(Field.Key); Values.Add(MoveTemp(Value));
	}
	const FString StructPath = ScriptStruct.GetPathName();
	if (!CheckString(Context, StructPath)) return false;
	OutValue = ObjectValue({TEXT("Properties"), TEXT("Struct")},
		{ObjectValue(MoveTemp(Keys), MoveTemp(Values)), StringValue(StructPath)});
	return true;
}

bool ReadEnum(FContext& Context, const UEnum* Enum, const int64 NativeValue,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	if (Enum == nullptr || Enum->HasAnyEnumFlags(EEnumFlags::Flags)) return Fail(Context, EFailure::InvalidEnum);
	const int32 Index = Enum->GetIndexByValue(NativeValue);
	const FString Enumerator = Enum->GetNameStringByValue(NativeValue);
	const FString Path = Enum->GetPathName();
	if (Index == INDEX_NONE || Enumerator.IsEmpty() || Enumerator == TEXT("MAX")
		|| Enumerator.EndsWith(TEXT("_MAX")) || Enum->HasMetaData(TEXT("Hidden"), Index)
		|| Enum->HasMetaData(TEXT("Spacer"), Index) || !CheckString(Context, Enumerator)
		|| !CheckString(Context, Path) || !ConsumeNode(Context, 3))
		return Fail(Context, EFailure::InvalidEnum);
	OutValue = ObjectValue({TEXT("Enum"), TEXT("Enumerator")},
		{StringValue(Path), StringValue(Enumerator)});
	return true;
}

bool ReadReference(FContext& Context, const TCHAR* Kind, const FString& Path,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	if (Path.IsEmpty()) { if (!ConsumeNode(Context)) return false; OutValue = Scalar(EAssetDocumentRuntimeValueKind::Null); return true; }
	const FSoftObjectPath Canonical(Path);
	if (!Canonical.IsValid() || !Canonical.GetSubPathString().IsEmpty()
		|| Canonical.ToString() != Path || !CheckString(Context, Path))
	{
		return Fail(Context, EFailure::UnstableReference);
	}
	if (!ConsumeNode(Context, 3)) return false;
	OutValue = ObjectValue({TEXT("Kind"), TEXT("Path")},
		{StringValue(Kind), StringValue(Path)});
	return true;
}

bool ReadArray(const FArrayProperty& Property, const void* Address, const int32 Depth,
	FContext& Context, FAssetDocumentRuntimeValueDto& OutValue)
{
	FScriptArrayHelper Array(&Property, Address);
	if (!ConsumeNode(Context) || !ConsumeContainer(Context, Array.Num())) return false;
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Array;
	OutValue.Elements.Reserve(Array.Num());
	for (int32 Index = 0; Index < Array.Num(); ++Index)
	{
		FAssetDocumentRuntimeValueDto Element;
		if (!ReadProperty(*Property.Inner, Array.GetElementPtr(Index), Depth + 1, Context, Element)) return false;
		OutValue.Elements.Add(MoveTemp(Element));
	}
	return true;
}

bool ReadSet(const FSetProperty& Property, const void* Address, const int32 Depth,
	FContext& Context, FAssetDocumentRuntimeValueDto& OutValue)
{
	const FScriptSetHelper Set(&Property, Address);
	if (!ConsumeNode(Context) || !ConsumeContainer(Context, Set.Num())) return false;
	TArray<TPair<TArray<uint8>, FAssetDocumentRuntimeValueDto>> Sorted;
	Sorted.Reserve(Set.Num());
	for (int32 Index = 0; Index < Set.GetMaxIndex(); ++Index)
	{
		if (!Set.IsValidIndex(Index)) continue;
		FAssetDocumentRuntimeValueDto Element; TArray<uint8> Key;
		if (!ReadProperty(*Property.ElementProp, Set.GetElementPtr(Index), Depth + 1, Context, Element)
			|| !MakeSortKey(Context, Element, Key)) return false;
		Sorted.Emplace(MoveTemp(Key), MoveTemp(Element));
	}
	Sorted.Sort([](const auto& Left, const auto& Right) { return BytesLess(Left.Key, Right.Key); });
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Array;
	for (int32 Index = 0; Index < Sorted.Num(); ++Index)
	{
		if (Index > 0 && Sorted[Index - 1].Key == Sorted[Index].Key)
			return Fail(Context, EFailure::CanonicalCollision);
		OutValue.Elements.Add(MoveTemp(Sorted[Index].Value));
	}
	return true;
}

bool ReadMap(const FMapProperty& Property, const void* Address, const int32 Depth,
	FContext& Context, FAssetDocumentRuntimeValueDto& OutValue)
{
	FScriptMapHelper Map(&Property, Address);
	if (!ConsumeNode(Context) || !ConsumeContainer(Context, Map.Num())
		|| !ConsumeNode(Context, Map.Num())) return false;
	TArray<TPair<TArray<uint8>, FAssetDocumentRuntimeValueDto>> Sorted;
	Sorted.Reserve(Map.Num());
	for (int32 Index = 0; Index < Map.GetMaxIndex(); ++Index)
	{
		if (!Map.IsValidIndex(Index)) continue;
		FAssetDocumentRuntimeValueDto Key; FAssetDocumentRuntimeValueDto Value; TArray<uint8> Bytes;
		if (!ReadProperty(*Property.KeyProp, Map.GetKeyPtr(Index), Depth + 1, Context, Key)
			|| !ReadProperty(*Property.ValueProp, Map.GetValuePtr(Index), Depth + 1, Context, Value)
			|| !MakeSortKey(Context, Key, Bytes)) return false;
		Sorted.Emplace(MoveTemp(Bytes), ObjectValue({TEXT("Key"), TEXT("Value")},
			{MoveTemp(Key), MoveTemp(Value)}));
	}
	Sorted.Sort([](const auto& Left, const auto& Right) { return BytesLess(Left.Key, Right.Key); });
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Array;
	for (int32 Index = 0; Index < Sorted.Num(); ++Index)
	{
		if (Index > 0 && Sorted[Index - 1].Key == Sorted[Index].Key)
			return Fail(Context, EFailure::CanonicalCollision);
		OutValue.Elements.Add(MoveTemp(Sorted[Index].Value));
	}
	return true;
}

bool ReadProperty(const FProperty& Property, const void* Address, const int32 Depth,
	FContext& Context, FAssetDocumentRuntimeValueDto& OutValue)
{
	if (Address == nullptr) return Fail(Context, EFailure::InvalidContext);
	if (Depth > MaximumDepth) return Fail(Context, EFailure::BudgetExceeded);
	if (const FArrayProperty* Typed = CastField<FArrayProperty>(&Property)) return ReadArray(*Typed, Address, Depth, Context, OutValue);
	if (const FSetProperty* Typed = CastField<FSetProperty>(&Property)) return ReadSet(*Typed, Address, Depth, Context, OutValue);
	if (const FMapProperty* Typed = CastField<FMapProperty>(&Property)) return ReadMap(*Typed, Address, Depth, Context, OutValue);
	if (const FStructProperty* Typed = CastField<FStructProperty>(&Property))
	{
		if (Typed->Struct == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct& Dynamic = *static_cast<const FInstancedStruct*>(Address);
			if (!Dynamic.IsValid()) { if (!ConsumeNode(Context)) return false; OutValue = Scalar(EAssetDocumentRuntimeValueKind::Null); return true; }
			return ReadStruct(*Dynamic.GetScriptStruct(), Dynamic.GetMemory(), Depth, Context, OutValue);
		}
		return Typed->Struct != nullptr && ReadStruct(*Typed->Struct, Address, Depth, Context, OutValue);
	}
	if (const FEnumProperty* Typed = CastField<FEnumProperty>(&Property))
	{
		const FNumericProperty* Underlying = Typed->GetUnderlyingProperty();
		return ReadEnum(Context, Typed->GetEnum(), Underlying->GetSignedIntPropertyValue(Address), OutValue);
	}
	if (const FNumericProperty* Typed = CastField<FNumericProperty>(&Property))
	{
		if (UEnum* Enum = Typed->GetIntPropertyEnum())
			return ReadEnum(Context, Enum, Typed->GetSignedIntPropertyValue(Address), OutValue);
		if (Typed->IsFloatingPoint())
		{
			const double Value = Typed->GetFloatingPointPropertyValue(Address);
			if (!FMath::IsFinite(Value)) return Fail(Context, EFailure::NonFiniteNumber);
			if (!ConsumeNode(Context)) return false;
			OutValue = Scalar(EAssetDocumentRuntimeValueKind::Number);
			OutValue.NumberValue = Value == 0.0 ? 0.0 : Value; return true;
		}
		if (CastField<FUInt64Property>(&Property) != nullptr)
		{
			if (!ConsumeNode(Context, 3)) return false;
			OutValue = ObjectValue({TEXT("Kind"), TEXT("Value")},
				{StringValue(TEXT("UInt64")), StringValue(LexToString(Typed->GetUnsignedIntPropertyValue(Address)))});
			return true;
		}
		if (!ConsumeNode(Context)) return false;
		OutValue = Scalar(EAssetDocumentRuntimeValueKind::Integer);
		OutValue.IntegerValue = CastField<FInt8Property>(&Property) || CastField<FInt16Property>(&Property)
			|| CastField<FIntProperty>(&Property) || CastField<FInt64Property>(&Property)
			? Typed->GetSignedIntPropertyValue(Address)
			: static_cast<int64>(Typed->GetUnsignedIntPropertyValue(Address));
		return true;
	}
	if (const FBoolProperty* Typed = CastField<FBoolProperty>(&Property))
	{
		if (!ConsumeNode(Context)) return false;
		OutValue = Scalar(EAssetDocumentRuntimeValueKind::Boolean);
		OutValue.BooleanValue = Typed->GetPropertyValue(Address); return true;
	}
	if (const FStrProperty* Typed = CastField<FStrProperty>(&Property))
	{
		const FString& Value = Typed->GetPropertyValue(Address);
		if (!ConsumeNode(Context) || !CheckString(Context, Value)) return false;
		OutValue = StringValue(Value); return true;
	}
	if (const FNameProperty* Typed = CastField<FNameProperty>(&Property))
	{
		const FName Value = Typed->GetPropertyValue(Address);
		const FString Spelling = Value.IsNone() ? FString() : Value.ToString();
		if (!ConsumeNode(Context) || !CheckString(Context, Spelling)) return false;
		if (!Value.IsNone() && FName(*Spelling).ToString() != Spelling)
			return Fail(Context, EFailure::AmbiguousName);
		OutValue = StringValue(Spelling); return true;
	}
	if (const FTextProperty* Typed = CastField<FTextProperty>(&Property))
	{
		const FText& Text = Typed->GetPropertyValue(Address); FName Table; FString TableKey;
		const FTextId Id = FTextInspector::GetTextId(Text);
		TArray<FHistoricTextFormatData> Formats; FHistoricTextNumericData Numeric;
		FTextInspector::GetHistoricFormatData(Text, Formats);
		const uint32 Flags = FTextInspector::GetFlags(Text);
		const uint32 Allowed = ETextFlag::CultureInvariant | ETextFlag::InitializedFromString;
		const FString* Source = FTextInspector::GetSourceString(Text);
		if (FTextInspector::GetTableIdAndKey(Text, Table, TableKey)
			|| !Id.GetNamespace().IsEmpty() || !Id.GetKey().IsEmpty()
			|| (FTextInspector::GetNamespace(Text).IsSet()
				&& !FTextInspector::GetNamespace(Text).GetValue().IsEmpty())
			|| (FTextInspector::GetKey(Text).IsSet()
				&& !FTextInspector::GetKey(Text).GetValue().IsEmpty())
			|| FTextInspector::ShouldGatherForLocalization(Text) || !Formats.IsEmpty()
			|| FTextInspector::GetHistoricNumericData(Text, Numeric) || (Flags & ~Allowed) != 0
			|| Source == nullptr || (!Source->IsEmpty() && (Flags & ETextFlag::CultureInvariant) == 0)
			|| !ConsumeNode(Context) || !CheckString(Context, *Source))
			return Fail(Context, EFailure::UnstableText);
		OutValue = StringValue(*Source); return true;
	}
	if (const FSoftClassProperty* Typed = CastField<FSoftClassProperty>(&Property))
		return ReadReference(Context, TEXT("SoftClassRef"), Typed->GetPropertyValue(Address).ToSoftObjectPath().ToString(), OutValue);
	if (const FSoftObjectProperty* Typed = CastField<FSoftObjectProperty>(&Property))
		return ReadReference(Context, TEXT("SoftAssetRef"), Typed->GetPropertyValue(Address).ToSoftObjectPath().ToString(), OutValue);
	if (const FClassProperty* Typed = CastField<FClassProperty>(&Property))
	{
		UClass* Value = Cast<UClass>(Typed->GetObjectPropertyValue(Address));
		if (Value == nullptr) return ReadReference(Context, TEXT("ClassRef"), FString(), OutValue);
		const UPackage* Package = Value->GetOutermost();
		const bool bPersistentNativeClass = Value->HasAnyClassFlags(CLASS_Native)
			&& Value->GetPathName().StartsWith(TEXT("/Script/"));
		if (Typed->PropertyClass == nullptr || !Value->GetClass()->IsChildOf(Typed->PropertyClass)
			|| Typed->MetaClass == nullptr || !Value->IsChildOf(Typed->MetaClass)
			|| Package == nullptr || Value->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| (!bPersistentNativeClass && (Value->HasAnyFlags(RF_Transient)
				|| Package->HasAnyFlags(RF_Transient)))) return Fail(Context, EFailure::UnstableReference);
		return ReadReference(Context, TEXT("ClassRef"), Value->GetPathName(), OutValue);
	}
	if (const FObjectProperty* Typed = CastField<FObjectProperty>(&Property))
	{
		UObject* Value = Typed->GetObjectPropertyValue(Address);
		if (Value == nullptr) return ReadReference(Context, TEXT("AssetRef"), FString(), OutValue);
		const UPackage* Package = Value->GetOutermost();
		if (Typed->PropertyClass == nullptr || !Value->IsA(Typed->PropertyClass)
			|| Package == nullptr || Value->HasAnyFlags(RF_Transient | RF_BeginDestroyed | RF_FinishDestroyed)
			|| Package->HasAnyFlags(RF_Transient)) return Fail(Context, EFailure::UnstableReference);
		return ReadReference(Context, TEXT("AssetRef"), Value->GetPathName(), OutValue);
	}
	return Fail(Context, EFailure::UnsupportedProperty);
}

bool IsSafeEditorContext(const UBlackboardKeyType_Struct* Owner)
{
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet() || IsGarbageCollecting()) return false;
	if (Owner == nullptr) return true;
	const UPackage* Package = Owner->GetOutermost();
	return Owner->GetClass() == UBlackboardKeyType_Struct::StaticClass() && Package != nullptr
		&& !Owner->HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed)
		&& !Owner->HasAnyInternalFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable)
		&& !Package->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& !Package->HasAnyInternalFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable);
}

bool HasExactLiveDefaultValue(const FInstancedStruct& Value, const UBlackboardKeyType_Struct& Owner)
{
	const FStructProperty* Property = FindFProperty<FStructProperty>(
		UBlackboardKeyType_Struct::StaticClass(), GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	return Property != nullptr && Property->GetOwnerStruct() == UBlackboardKeyType_Struct::StaticClass()
		&& Property->Struct == FInstancedStruct::StaticStruct() && &Value == &Owner.DefaultValue;
}
} // namespace

FReadResult ReadCanonicalValue(const FInstancedStruct& Value, const ESource Source,
	const UBlackboardKeyType_Struct* ExactLiveOwner)
{
	using namespace AIBlackboardKeyTypeStructCanonicalReadLocal;
	FReadResult Result;
	if (!IsSafeEditorContext(ExactLiveOwner)
		|| (Source != ESource::ExactLiveDefaultValue && Source != ESource::DetachedScratch)
		|| (Source == ESource::ExactLiveDefaultValue
			&& (ExactLiveOwner == nullptr || !HasExactLiveDefaultValue(Value, *ExactLiveOwner)))
		|| (Source == ESource::DetachedScratch && ExactLiveOwner != nullptr))
		return {false, EFailure::InvalidContext, {}};
	if (!Value.IsValid())
		return {true, EFailure::None, {}};
	const UScriptStruct* ScriptStruct = Value.GetScriptStruct();
	const void* Memory = Value.GetMemory();
	if (ScriptStruct == nullptr || Memory == nullptr)
		return {false, EFailure::SchemaDrift, {}};
	Private::FSchemaSnapshot BeforeSchema;
	EFailure SchemaFailure = EFailure::None;
	if (!Private::CaptureSchemaSnapshot(*ScriptStruct, Memory, BeforeSchema, SchemaFailure))
	{
		Result.Failure = SchemaFailure; return Result;
	}
	FContext Context;
	FAssetDocumentRuntimeValueDto Scratch;
	if (!ReadStruct(*ScriptStruct, Memory, 0, Context, Scratch))
	{
		Result.Failure = Context.Failure == EFailure::None ? EFailure::SchemaDrift : Context.Failure;
		return Result;
	}
	Private::FSchemaSnapshot AfterSchema;
	if (Value.GetScriptStruct() != ScriptStruct || Value.GetMemory() != Memory
		|| !Private::CaptureSchemaSnapshot(*ScriptStruct, Memory, AfterSchema, SchemaFailure)
		|| !(BeforeSchema == AfterSchema))
	{
		Result.Failure = SchemaFailure == EFailure::None ? EFailure::SchemaDrift : SchemaFailure;
		return Result;
	}
	if (Private::ShouldForceReadSchemaDrift()) { Result.Failure = EFailure::SchemaDrift; return Result; }
	TArray<uint8> CanonicalBytes;
	if (!EncodeCanonical(Scratch, CanonicalBytes, Context.RemainingComparisonBytes))
	{
		Result.Failure = EFailure::BudgetExceeded; return Result;
	}
	if (!IsAssetDocumentAICanonicalRuntimeValue(Scratch))
	{
		Result.Failure = EFailure::DtoInvalid; return Result;
	}
	Result.bSucceeded = true; Result.Failure = EFailure::None; Result.Value = MoveTemp(Scratch);
	return Result;
}
} // namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalRead
