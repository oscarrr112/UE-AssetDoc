// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalRead.h"

#include "StructUtils/InstancedStruct.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::Private
{
namespace
{
constexpr int32 MaximumSchemaDepth = 16;
constexpr int32 MaximumSchemaFields = 256;
constexpr int32 MaximumSchemaContainerEntries = 1024;
constexpr int32 MaximumSchemaStringBytes = 256 * 1024;

struct FSchemaState
{
	FSchemaSnapshot Snapshot;
	EFailure Failure = EFailure::None;
	FReadBudgetPrecharge Precharge = GetReadBudgetPrecharge();
	int32 RemainingSchemaFields = MaximumSchemaFields - Precharge.SchemaFields;
	int32 RemainingFields = MaximumSchemaFields - Precharge.Fields;
	int32 RemainingContainerEntries = MaximumSchemaContainerEntries - Precharge.ContainerEntries;
	TSet<const UScriptStruct*> CompletedTypes;
	TArray<TPair<const UScriptStruct*, const void*>> ActiveInstances;
};

bool Fail(FSchemaState& State, const EFailure Failure)
{
	if (State.Failure == EFailure::None) State.Failure = Failure;
	return false;
}

bool IsStableString(const FString& Value)
{
	const FTCHARToUTF8 Utf8(*Value, Value.Len());
	FAssetDocumentRuntimeValueDto String;
	String.Kind = EAssetDocumentRuntimeValueKind::String;
	String.StringValue = Value;
	return Utf8.Length() <= MaximumSchemaStringBytes
		&& ValidateAssetDocumentRuntimeValue(String).bValid;
}

void AddStructToken(FSchemaState& State, const ESchemaTokenKind Kind,
	const UStruct& Struct, const void* Identity = nullptr)
{
	FSchemaToken& Token = State.Snapshot.Tokens.AddDefaulted_GetRef();
	Token.Kind = Kind;
	Token.Identity = reinterpret_cast<UPTRINT>(Identity == nullptr ? &Struct : Identity);
	Token.TypeA = reinterpret_cast<UPTRINT>(&Struct);
	Token.Name = Struct.GetPathName();
}

void AddContainerToken(FSchemaState& State, const FProperty& Property, const int32 Count)
{
	FSchemaToken& Token = State.Snapshot.Tokens.AddDefaulted_GetRef();
	Token.Kind = ESchemaTokenKind::Container;
	Token.Identity = reinterpret_cast<UPTRINT>(&Property);
	Token.TypeA = static_cast<UPTRINT>(Count);
}

bool CaptureStaticType(const UScriptStruct&, int32, FSchemaState&);

bool CapturePropertyType(const FProperty& Property, const int32 Depth, FSchemaState& State)
{
	if (Depth > MaximumSchemaDepth) return Fail(State, EFailure::BudgetExceeded);
	FSchemaToken& Token = State.Snapshot.Tokens.AddDefaulted_GetRef();
	Token.Kind = ESchemaTokenKind::Property;
	Token.Identity = reinterpret_cast<UPTRINT>(&Property);
	Token.TypeA = reinterpret_cast<UPTRINT>(Property.GetClass());
	Token.TypeB = reinterpret_cast<UPTRINT>(Property.GetOwnerStruct());
	Token.Flags = static_cast<uint64>(Property.GetPropertyFlags());
	Token.ArrayDim = Property.ArrayDim;
	Token.Name = Property.GetAuthoredName();
	Token.OwnerPath = Property.GetOwnerStruct() == nullptr
		? FString() : Property.GetOwnerStruct()->GetPathName();
	if (!IsStableString(Token.Name) || !IsStableString(Token.OwnerPath))
		return Fail(State, EFailure::UnsupportedProperty);

	if (const FArrayProperty* Typed = CastField<FArrayProperty>(&Property))
		return Typed->Inner != nullptr && CapturePropertyType(*Typed->Inner, Depth + 1, State);
	if (const FSetProperty* Typed = CastField<FSetProperty>(&Property))
		return Typed->ElementProp != nullptr
			&& CapturePropertyType(*Typed->ElementProp, Depth + 1, State);
	if (const FMapProperty* Typed = CastField<FMapProperty>(&Property))
		return Typed->KeyProp != nullptr && Typed->ValueProp != nullptr
			&& CapturePropertyType(*Typed->KeyProp, Depth + 1, State)
			&& CapturePropertyType(*Typed->ValueProp, Depth + 1, State);
	if (const FStructProperty* Typed = CastField<FStructProperty>(&Property))
	{
		if (Typed->Struct == nullptr) return Fail(State, EFailure::UnsupportedProperty);
		Token.TypeC = reinterpret_cast<UPTRINT>(Typed->Struct.Get());
		Token.TypePathA = Typed->Struct->GetPathName();
		return IsStableString(Token.TypePathA)
			&& (Typed->Struct == FInstancedStruct::StaticStruct()
				|| CaptureStaticType(*Typed->Struct, Depth + 1, State));
	}
	if (const FEnumProperty* Typed = CastField<FEnumProperty>(&Property))
	{
		if (Typed->GetEnum() == nullptr || Typed->GetUnderlyingProperty() == nullptr
			|| Typed->GetEnum()->HasAnyEnumFlags(EEnumFlags::Flags))
			return Fail(State, EFailure::UnsupportedProperty);
		Token.TypeC = reinterpret_cast<UPTRINT>(Typed->GetEnum());
		Token.TypeB = reinterpret_cast<UPTRINT>(Typed->GetUnderlyingProperty()->GetClass());
		Token.TypePathA = Typed->GetEnum()->GetPathName();
		return IsStableString(Token.TypePathA);
	}
	if (const FNumericProperty* Typed = CastField<FNumericProperty>(&Property))
	{
		if (UEnum* Enum = Typed->GetIntPropertyEnum())
		{
			if (Enum->HasAnyEnumFlags(EEnumFlags::Flags))
				return Fail(State, EFailure::UnsupportedProperty);
			Token.TypeC = reinterpret_cast<UPTRINT>(Enum);
			Token.TypePathA = Enum->GetPathName();
			return IsStableString(Token.TypePathA);
		}
		return Typed->IsInteger() || Typed->IsFloatingPoint();
	}
	if (CastField<FBoolProperty>(&Property) || CastField<FStrProperty>(&Property)
		|| CastField<FNameProperty>(&Property) || CastField<FTextProperty>(&Property)) return true;
	if (const FSoftClassProperty* Typed = CastField<FSoftClassProperty>(&Property))
	{
		if (Typed->PropertyClass == nullptr || Typed->MetaClass == nullptr)
			return Fail(State, EFailure::UnsupportedProperty);
		Token.TypeC = reinterpret_cast<UPTRINT>(Typed->PropertyClass.Get());
		Token.TypeB = reinterpret_cast<UPTRINT>(Typed->MetaClass.Get());
		Token.TypePathA = Typed->PropertyClass->GetPathName();
		Token.TypePathB = Typed->MetaClass->GetPathName();
		return IsStableString(Token.TypePathA) && IsStableString(Token.TypePathB);
	}
	if (const FSoftObjectProperty* Typed = CastField<FSoftObjectProperty>(&Property))
	{
		if (Typed->PropertyClass == nullptr) return Fail(State, EFailure::UnsupportedProperty);
		Token.TypeC = reinterpret_cast<UPTRINT>(Typed->PropertyClass.Get());
		Token.TypePathA = Typed->PropertyClass->GetPathName();
		return IsStableString(Token.TypePathA);
	}
	if (const FClassProperty* Typed = CastField<FClassProperty>(&Property))
	{
		if (Typed->PropertyClass == nullptr || Typed->MetaClass == nullptr)
			return Fail(State, EFailure::UnsupportedProperty);
		Token.TypeC = reinterpret_cast<UPTRINT>(Typed->PropertyClass.Get());
		Token.TypeB = reinterpret_cast<UPTRINT>(Typed->MetaClass.Get());
		Token.TypePathA = Typed->PropertyClass->GetPathName();
		Token.TypePathB = Typed->MetaClass->GetPathName();
		return IsStableString(Token.TypePathA) && IsStableString(Token.TypePathB);
	}
	if (const FObjectProperty* Typed = CastField<FObjectProperty>(&Property))
	{
		if (Typed->PropertyClass == nullptr) return Fail(State, EFailure::UnsupportedProperty);
		Token.TypeC = reinterpret_cast<UPTRINT>(Typed->PropertyClass.Get());
		Token.TypePathA = Typed->PropertyClass->GetPathName();
		return IsStableString(Token.TypePathA);
	}
	return Fail(State, EFailure::UnsupportedProperty);
}

bool CaptureStaticType(const UScriptStruct& ScriptStruct, const int32 Depth, FSchemaState& State)
{
	if (Depth > MaximumSchemaDepth) return Fail(State, EFailure::BudgetExceeded);
	AddStructToken(State, ESchemaTokenKind::Struct, ScriptStruct);
	if (!IsStableString(ScriptStruct.GetPathName()))
		return Fail(State, EFailure::UnsupportedProperty);
	for (const UStruct* Chain = &ScriptStruct; Chain != nullptr; Chain = Chain->GetSuperStruct())
	{
		AddStructToken(State, ESchemaTokenKind::Inheritance, *Chain);
		if (!IsStableString(Chain->GetPathName()))
			return Fail(State, EFailure::UnsupportedProperty);
	}
	if (State.CompletedTypes.Contains(&ScriptStruct)) return true;
	State.CompletedTypes.Add(&ScriptStruct);
	TArray<TPair<FString, const FProperty*>> Fields;
	if (!CollectStoredFields(ScriptStruct, State.RemainingSchemaFields, Fields, State.Failure))
		return false;
	for (const TPair<FString, const FProperty*>& Field : Fields)
		if (!CapturePropertyType(*Field.Value, Depth + 1, State)) return false;
	return true;
}

bool CaptureDynamicInstances(const UScriptStruct&, const void*, int32, FSchemaState&);

bool CapturePropertyInstances(const FProperty& Property, const void* Address,
	const int32 Depth, FSchemaState& State)
{
	if (Address == nullptr) return Fail(State, EFailure::SchemaDrift);
	if (Depth > MaximumSchemaDepth) return Fail(State, EFailure::BudgetExceeded);
	if (const FArrayProperty* Typed = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Array(Typed, Address);
		if (Array.Num() > MaximumSchemaContainerEntries
			|| State.RemainingContainerEntries < Array.Num())
			return Fail(State, EFailure::BudgetExceeded);
		State.RemainingContainerEntries -= Array.Num();
		AddContainerToken(State, Property, Array.Num());
		for (int32 Index = 0; Index < Array.Num(); ++Index)
			if (!CapturePropertyInstances(*Typed->Inner,
				Array.GetElementPtr(Index), Depth + 1, State)) return false;
		return true;
	}
	if (const FSetProperty* Typed = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper Set(Typed, Address);
		if (Set.Num() > MaximumSchemaContainerEntries
			|| State.RemainingContainerEntries < Set.Num())
			return Fail(State, EFailure::BudgetExceeded);
		State.RemainingContainerEntries -= Set.Num();
		AddContainerToken(State, Property, Set.Num());
		for (int32 Index = 0; Index < Set.GetMaxIndex(); ++Index)
			if (Set.IsValidIndex(Index) && !CapturePropertyInstances(
				*Typed->ElementProp, Set.GetElementPtr(Index), Depth + 1, State)) return false;
		return true;
	}
	if (const FMapProperty* Typed = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Map(Typed, Address);
		if (Map.Num() > MaximumSchemaContainerEntries
			|| State.RemainingContainerEntries < Map.Num())
			return Fail(State, EFailure::BudgetExceeded);
		State.RemainingContainerEntries -= Map.Num();
		AddContainerToken(State, Property, Map.Num());
		for (int32 Index = 0; Index < Map.GetMaxIndex(); ++Index)
		{
			if (!Map.IsValidIndex(Index)) continue;
			if (!CapturePropertyInstances(*Typed->KeyProp, Map.GetKeyPtr(Index), Depth + 1, State)
				|| !CapturePropertyInstances(*Typed->ValueProp,
					Map.GetValuePtr(Index), Depth + 1, State)) return false;
		}
		return true;
	}
	if (const FStructProperty* Typed = CastField<FStructProperty>(&Property))
	{
		if (Typed->Struct == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct& Dynamic = *static_cast<const FInstancedStruct*>(Address);
			if (!Dynamic.IsValid()) return true;
			const UScriptStruct* DynamicType = Dynamic.GetScriptStruct();
			const void* DynamicMemory = Dynamic.GetMemory();
			if (DynamicType == nullptr || DynamicMemory == nullptr)
				return Fail(State, EFailure::SchemaDrift);
			AddStructToken(State, ESchemaTokenKind::DynamicStruct, *DynamicType, DynamicMemory);
			return CaptureStaticType(*DynamicType, Depth, State)
				&& CaptureDynamicInstances(*DynamicType, DynamicMemory, Depth, State);
		}
		return Typed->Struct != nullptr
			&& CaptureDynamicInstances(*Typed->Struct, Address, Depth, State);
	}
	return true;
}

bool CaptureDynamicInstances(const UScriptStruct& ScriptStruct, const void* Memory,
	const int32 Depth, FSchemaState& State)
{
	if (Memory == nullptr) return Fail(State, EFailure::SchemaDrift);
	if (Depth > MaximumSchemaDepth) return Fail(State, EFailure::BudgetExceeded);
	const TPair<const UScriptStruct*, const void*> Identity(&ScriptStruct, Memory);
	if (State.ActiveInstances.Contains(Identity)) return Fail(State, EFailure::SchemaDrift);
	State.ActiveInstances.Add(Identity);
	TArray<TPair<FString, const FProperty*>> Fields;
	if (!CollectStoredFields(ScriptStruct, State.RemainingFields, Fields, State.Failure))
	{
		State.ActiveInstances.Pop();
		return false;
	}
	for (const TPair<FString, const FProperty*>& Field : Fields)
	{
		for (int32 Index = 0; Index < Field.Value->ArrayDim; ++Index)
		{
			if (!CapturePropertyInstances(*Field.Value,
				Field.Value->ContainerPtrToValuePtr<void>(Memory, Index), Depth + 1, State))
			{
				State.ActiveInstances.Pop();
				return false;
			}
		}
	}
	State.ActiveInstances.Pop();
	return true;
}
} // namespace

#if WITH_DEV_AUTOMATION_TESTS
const FReadTestControl* GActiveReadTestControl = nullptr;

FScopedReadTestControl::FScopedReadTestControl(const FReadTestControl& Control)
	: Previous(GActiveReadTestControl)
{
	GActiveReadTestControl = &Control;
}

FScopedReadTestControl::~FScopedReadTestControl()
{
	GActiveReadTestControl = Previous;
}
#endif

FReadBudgetPrecharge GetReadBudgetPrecharge()
{
#if WITH_DEV_AUTOMATION_TESTS
	if (GActiveReadTestControl != nullptr)
	{
		return {GActiveReadTestControl->PrechargeSchemaFields,
			GActiveReadTestControl->PrechargeFields,
			GActiveReadTestControl->PrechargeContainerEntries,
			GActiveReadTestControl->PrechargeNodes,
			GActiveReadTestControl->PrechargeComparisonBytes};
	}
#endif
	return {};
}

bool ShouldForceReadSchemaDrift()
{
#if WITH_DEV_AUTOMATION_TESTS
	return GActiveReadTestControl != nullptr && GActiveReadTestControl->bForceSchemaDrift;
#else
	return false;
#endif
}

bool FSchemaToken::operator==(const FSchemaToken& Other) const
{
	return Kind == Other.Kind && Identity == Other.Identity && TypeA == Other.TypeA
		&& TypeB == Other.TypeB && TypeC == Other.TypeC && Flags == Other.Flags
		&& ArrayDim == Other.ArrayDim && Name == Other.Name && OwnerPath == Other.OwnerPath
		&& TypePathA == Other.TypePathA && TypePathB == Other.TypePathB;
}

bool FSchemaSnapshot::operator==(const FSchemaSnapshot& Other) const
{
	return Tokens == Other.Tokens;
}

bool CollectStoredFields(const UScriptStruct& ScriptStruct, int32& RemainingFields,
	TArray<TPair<FString, const FProperty*>>& OutFields, EFailure& OutFailure)
{
	const EPropertyFlags Forbidden = CPF_Transient | CPF_DuplicateTransient
		| CPF_NonPIEDuplicateTransient | CPF_SkipSerialization | CPF_InstancedReference
		| CPF_ContainsInstancedReference;
	for (TFieldIterator<FProperty> It(&ScriptStruct,
		EFieldIterationFlags::IncludeSuper | EFieldIterationFlags::IncludeDeprecated); It; ++It)
	{
		const FProperty& Property = **It;
		const FString AuthoredName = Property.GetAuthoredName();
		if (RemainingFields <= 0)
		{
			OutFailure = EFailure::BudgetExceeded;
			return false;
		}
		--RemainingFields;
		if (Property.GetOwnerStruct() == nullptr || AuthoredName.IsEmpty()
			|| Property.HasAnyPropertyFlags(Forbidden)
			|| Property.HasMetaData(TEXT("Bitmask")) || Property.HasMetaData(TEXT("BitmaskEnum")))
		{
			OutFailure = EFailure::UnsupportedProperty;
			return false;
		}
		OutFields.Emplace(AuthoredName, &Property);
	}
	OutFields.Sort([](const auto& Left, const auto& Right) { return Left.Key < Right.Key; });
	for (int32 Index = 1; Index < OutFields.Num(); ++Index)
	{
		if (OutFields[Index - 1].Key == OutFields[Index].Key)
		{
			OutFailure = EFailure::SchemaDrift;
			return false;
		}
	}
	return true;
}

bool CaptureSchemaSnapshot(const UScriptStruct& ScriptStruct, const void* Memory,
	FSchemaSnapshot& OutSnapshot, EFailure& OutFailure)
{
	FSchemaState State;
	if (!CaptureStaticType(ScriptStruct, 0, State)
		|| !CaptureDynamicInstances(ScriptStruct, Memory, 0, State))
	{
		OutFailure = State.Failure == EFailure::None ? EFailure::SchemaDrift : State.Failure;
		return false;
	}
	OutSnapshot = MoveTemp(State.Snapshot);
	OutFailure = EFailure::None;
	return true;
}
} // namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::Private
