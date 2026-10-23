// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Blackboard/AssetDocumentAIBlackboardPrimitives.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalRead.h"

#include "Provider/AssetDocumentAIProviderContracts.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BlackboardData.h"
#include "Editor.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"

namespace AIBlackboardInspectionLocal
{
FAssetDocumentRuntimeValueDto StringValue(FString Value) {
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String; Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto IntegerValue(const int64 Value) {
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer; Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto NumberValue(const double Value) {
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number; Result.NumberValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto BooleanValue(const bool Value) {
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean; Result.BooleanValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto NullValue() {
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object; Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto ArrayValue(TArray<FAssetDocumentRuntimeValueDto> Values) {
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array; Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message)
{
	return MakeAssetDocumentAIPrimitiveFailure(
		EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message);
}

UBlackboardData* FindBlackboard(const FString& Path)
{
	UBlackboardData* Blackboard = Cast<UBlackboardData>(FSoftObjectPath(Path).ResolveObject());
	return Blackboard != nullptr && Blackboard->GetPathName() == Path ? Blackboard : nullptr;
}

UBlackboardKeyType* FindKeyType(UBlackboardData& Blackboard, const FString& Name)
{
	for (const FBlackboardEntry& Entry : Blackboard.Keys) {
		if (Entry.EntryName.ToString() == Name) return Entry.KeyType.Get(); }
	return nullptr;
}

bool IsAdmittedLeaf(const FProperty& Property, const IPropertyHandle& Handle)
{
	return Property.GetOwnerProperty() == &Property
		&& Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_Transient | CPF_EditConst | CPF_InstancedReference)
		&& Handle.IsValidHandle() && Handle.IsEditable()
		&& CastField<FArrayProperty>(&Property) == nullptr
		&& CastField<FSetProperty>(&Property) == nullptr
		&& CastField<FMapProperty>(&Property) == nullptr;
}

bool ReadLeaf(const IPropertyHandle& Handle, FAssetDocumentRuntimeValueDto& OutValue, FString& OutKind)
{
	const FProperty* Property = Handle.GetProperty();
	if (Property == nullptr) return false;
	if (CastField<FBoolProperty>(Property) != nullptr) {
		bool Value = false; if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = BooleanValue(Value); OutKind = TEXT("boolean"); return true; }
	if (CastField<FIntProperty>(Property) != nullptr) {
		int32 Value = 0; if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = IntegerValue(Value); OutKind = TEXT("integer"); return true; }
	if (CastField<FInt64Property>(Property) != nullptr) {
		int64 Value = 0; if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = IntegerValue(Value); OutKind = TEXT("integer"); return true; }
	if (CastField<FByteProperty>(Property) != nullptr) {
		uint8 Value = 0; if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = IntegerValue(Value); OutKind = TEXT("integer"); return true; }
	if (CastField<FFloatProperty>(Property) != nullptr) {
		float Value = 0.0f; if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = NumberValue(Value); OutKind = TEXT("number"); return FMath::IsFinite(Value); }
	if (CastField<FDoubleProperty>(Property) != nullptr) {
		double Value = 0.0; if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = NumberValue(Value); OutKind = TEXT("number"); return FMath::IsFinite(Value); }
	if (CastField<FStrProperty>(Property) != nullptr) {
		FString Value; if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = StringValue(MoveTemp(Value)); OutKind = TEXT("string"); return true; }
	if (CastField<FNameProperty>(Property) != nullptr) {
		FName Value; if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = StringValue(Value.ToString()); OutKind = TEXT("name"); return true; }
	return false;
}

bool IsEditorPropertyContext(const UObject* Object = nullptr)
{
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet() || IsGarbageCollecting()) return false;
	if (Object == nullptr) return true;
	const UPackage* Package = Object->GetOutermost();
	return Package != nullptr
		&& !Object->HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed)
		&& !Object->HasAnyInternalFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable)
		&& !Package->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& !Package->HasAnyInternalFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable);
}

bool TryMakeRows(UObject& Object, TSharedPtr<IPropertyRowGenerator>& OutRows)
{
	if (!IsEditorPropertyContext(&Object)) return false;
	FPropertyEditorModule* PropertyEditor = FModuleManager::LoadModulePtr<FPropertyEditorModule>(
		TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr) return false;
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = false;
	OutRows = PropertyEditor->CreatePropertyRowGenerator(Args);
	OutRows->OnRowsRefreshed().AddLambda([]() {});
	#if WITH_DEV_AUTOMATION_TESTS
	AssetDocumentAIBlackboardMutation::RecordBlackboardMetadataGenerator(Object, nullptr);
	#endif
	OutRows->SetObjects({&Object});
	return true;
}

bool GatherTopLevelHandles(const TArray<TSharedRef<IDetailTreeNode>>& Nodes, UObject& Owner,
	TMap<FString, TSharedRef<IPropertyHandle>>& OutHandles, const bool bRejectUnsupported = false)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes) {
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid() && Handle->GetProperty() != nullptr) {
			const FProperty& Property = *Handle->GetProperty();
			if (Property.GetOwnerProperty() == &Property && Property.HasAnyPropertyFlags(CPF_Edit)
				&& !Property.HasAnyPropertyFlags(CPF_Transient | CPF_EditConst | CPF_InstancedReference)) {
				if (!IsAdmittedLeaf(Property, *Handle) && bRejectUnsupported) return false;
				if (IsAdmittedLeaf(Property, *Handle)) {
					TArray<UObject*> Outers;
					Handle->GetOuterObjects(Outers);
					if (Outers.Num() == 1 && Outers[0] == &Owner)
						OutHandles.Add(Property.GetName(), Handle.ToSharedRef());
				} }
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		if (!GatherTopLevelHandles(Children, Owner, OutHandles, bRejectUnsupported)) return false;
	}
	return true;
}
} // namespace

namespace AIBlackboardInspectionLocal
{
struct FKeyTypeInspection
{
	UBlackboardKeyType* KeyType = nullptr;
	TSharedPtr<IPropertyRowGenerator> Rows;
};

bool ResolveKeyTypeInspection(const FAssetDocumentPrimitiveInvocation& Invocation,
	FKeyTypeInspection& OutInspection, FAssetDocumentPrimitiveOutcome& OutFailure)
{
	FAssetDocumentAIBlackboardKeyTypeRequest Request;
	if (!ParseAssetDocumentAIBlackboardKeyTypeRequest(Invocation, false, Request)) {
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The Blackboard key-type request is not canonical."));
		return false; }
	UBlackboardData* Blackboard = FindBlackboard(Request.BlackboardPath);
	OutInspection.KeyType = Blackboard != nullptr
		? FindKeyType(*Blackboard, Request.KeyName) : nullptr;
	if (OutInspection.KeyType == nullptr) {
		OutFailure = Failure(TEXT("AIProvider.Blackboard.KeyMissing"), TEXT("The requested existing Blackboard key is unavailable."));
		return false; }
	if (OutInspection.KeyType->GetClass()->GetPathName() != Request.ExpectedClassPath) {
		OutFailure = Failure(TEXT("AIProvider.Blackboard.KeyTypeClassMismatch"), TEXT("The existing Blackboard key type does not match the requested exact class."));
		return false; }
	if (!IsEditorPropertyContext()) {
		OutFailure = Failure(TEXT("AIProvider.InvalidContext"), TEXT("Blackboard property evidence requires the Editor game thread."));
		return false; }
	if (!TryMakeRows(*OutInspection.KeyType, OutInspection.Rows)) {
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"), TEXT("The PropertyEditor module is unavailable."));
		return false; }
	return true;
}

bool CollectKeyTypeEvidence(const FKeyTypeInspection& Inspection,
	FAssetDocumentRuntimeValueDto& OutProperties, FAssetDocumentRuntimeValueDto& OutSchema,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	TMap<FString, TSharedRef<IPropertyHandle>> Handles;
	GatherTopLevelHandles(Inspection.Rows->GetRootTreeNodes(), *Inspection.KeyType, Handles);
	TArray<FString> Names;
	Handles.GetKeys(Names);
	Names.Sort();
	TArray<FString> PropertyKeys;
	TArray<FAssetDocumentRuntimeValueDto> PropertyValues;
	TArray<FAssetDocumentRuntimeValueDto> Schema;
	for (const FString& Name : Names) {
		FAssetDocumentRuntimeValueDto Value;
		FString Kind;
		if (!ReadLeaf(*Handles.FindChecked(Name), Value, Kind)) {
			OutFailure = Failure(TEXT("AIProvider.Blackboard.PropertyEvidenceUnavailable"), TEXT("An admitted Blackboard key-type property could not be read."));
			return false; }
		PropertyKeys.Add(Name);
		PropertyValues.Add(MoveTemp(Value));
		Schema.Add(ObjectValue({TEXT("kind"), TEXT("name")}, {StringValue(Kind), StringValue(Name)}));
	}
	OutProperties = ObjectValue(MoveTemp(PropertyKeys), MoveTemp(PropertyValues));
	OutSchema = ArrayValue(MoveTemp(Schema));
	return true;
}

UBlackboardData* ResolveExactBlackboard(const FString& Path)
{
	UObject* const Object = FSoftObjectPath(Path).ResolveObject();
	if (Object == nullptr || Object->GetClass() != UBlackboardData::StaticClass()
		|| Object->GetPathName() != Path) return nullptr;
	return static_cast<UBlackboardData*>(Object);
}

const FBlackboardEntry* FindOwnedLocalEntry(
	const UBlackboardData& Blackboard,
	const FName KeyName)
{
	const FBlackboardEntry* Match = nullptr;
	for (const FBlackboardEntry& Entry : Blackboard.Keys)
	{
		if (Entry.EntryName == KeyName)
		{
			if (Match != nullptr)
			{
				// Local Names are the primary identity.  Returning the second
				// duplicate keeps it distinct from GetKey's first visible entry,
				// so the handler rejects the residual ambiguous identity.
				return &Entry;
			}
			Match = &Entry;
		}
	}
	return Match;
}

FAssetDocumentRuntimeValueDto ClassReferenceValue(const UClass& Class)
{
	return ObjectValue(
		{TEXT("Kind"), TEXT("Path")},
		{StringValue(TEXT("ClassRef")), StringValue(Class.GetPathName())});
}

FAssetDocumentRuntimeValueDto AssetReferenceValue(const UObject& Object)
{
	return ObjectValue({TEXT("Kind"), TEXT("Path")},
		{StringValue(TEXT("AssetRef")), StringValue(Object.GetPathName())});
}

FAssetDocumentRuntimeValueDto AssetReferenceOrNull(const UObject* Object)
{
	return Object == nullptr ? NullValue() : AssetReferenceValue(*Object);
}

FAssetDocumentRuntimeValueDto ClassReferenceOrNull(const UClass* Class)
{
	return Class == nullptr ? NullValue() : ClassReferenceValue(*Class);
}

TArray<FString> DirectValueComponentNames(const UClass* KeyTypeClass)
{
	if (KeyTypeClass == UBlackboardKeyType_Vector::StaticClass())
	{
		return {TEXT("X"), TEXT("Y"), TEXT("Z")};
	}
	if (KeyTypeClass == UBlackboardKeyType_Rotator::StaticClass())
	{
		return {TEXT("Pitch"), TEXT("Roll"), TEXT("Yaw")};
	}
	return {};
}

// Vector and Rotator keys keep their default behind an inline edit condition
// (bUseDefaultValue is not a details row), so they use a direct exact codec:
// {"DefaultValue": null} when bUseDefaultValue is off (the stored value is unused),
// otherwise {"DefaultValue": {"X","Y","Z"} | {"Pitch","Roll","Yaw"}}.
bool ReadDirectValueKeyTypeProperties(
	const UBlackboardKeyType& KeyType, FAssetDocumentRuntimeValueDto& OutProperties)
{
	const TArray<FString> Names = DirectValueComponentNames(KeyType.GetClass());
	double Components[3] = {0.0, 0.0, 0.0};
	bool bUseDefaultValue = false;
	if (KeyType.GetClass() == UBlackboardKeyType_Vector::StaticClass())
	{
		const UBlackboardKeyType_Vector& Typed = static_cast<const UBlackboardKeyType_Vector&>(KeyType);
		Components[0] = Typed.DefaultValue.X;
		Components[1] = Typed.DefaultValue.Y;
		Components[2] = Typed.DefaultValue.Z;
		bUseDefaultValue = Typed.bUseDefaultValue;
	}
	else if (KeyType.GetClass() == UBlackboardKeyType_Rotator::StaticClass())
	{
		const UBlackboardKeyType_Rotator& Typed = static_cast<const UBlackboardKeyType_Rotator&>(KeyType);
		Components[0] = Typed.DefaultValue.Pitch;
		Components[1] = Typed.DefaultValue.Roll;
		Components[2] = Typed.DefaultValue.Yaw;
		bUseDefaultValue = Typed.bUseDefaultValue;
	}
	else
	{
		return false;
	}
	if (!bUseDefaultValue)
	{
		OutProperties = ObjectValue({TEXT("DefaultValue")}, {NullValue()});
		return true;
	}
	TArray<FAssetDocumentRuntimeValueDto> Values;
	for (int32 Index = 0; Index < Names.Num(); ++Index)
	{
		if (!FMath::IsFinite(Components[Index])) return false;
		Values.Add(NumberValue(Components[Index]));
	}
	OutProperties = ObjectValue({TEXT("DefaultValue")},
		{ObjectValue(TArray<FString>(Names), MoveTemp(Values))});
	return true;
}

bool CollectStockEntryReadbackProperties(
	UBlackboardKeyType& KeyType, FAssetDocumentRuntimeValueDto& OutProperties, bool& bHandled)
{
	bHandled = true;
	if (AssetDocumentAIBlackboardMutation::IsAssetDocumentAIBlackboardDirectValueKeyType(KeyType.GetClass()))
	{
		return ReadDirectValueKeyTypeProperties(KeyType, OutProperties);
	}
	if (KeyType.GetClass() == UBlackboardKeyType_Object::StaticClass())
	{
		const UBlackboardKeyType_Object& Typed = static_cast<const UBlackboardKeyType_Object&>(KeyType);
		if (Typed.BaseClass == nullptr || (Typed.DefaultValue != nullptr && !Typed.DefaultValue->IsA(Typed.BaseClass))) return false;
		OutProperties = ObjectValue({TEXT("BaseClass"), TEXT("DefaultValue")},
			{ClassReferenceValue(*Typed.BaseClass), AssetReferenceOrNull(Typed.DefaultValue)});
		return true;
	}
	if (KeyType.GetClass() == UBlackboardKeyType_Class::StaticClass())
	{
		const UBlackboardKeyType_Class& Typed = static_cast<const UBlackboardKeyType_Class&>(KeyType);
		if (Typed.BaseClass == nullptr || (Typed.DefaultValue != nullptr && !Typed.DefaultValue->IsChildOf(Typed.BaseClass))) return false;
		OutProperties = ObjectValue({TEXT("BaseClass"), TEXT("DefaultValue")},
			{ClassReferenceValue(*Typed.BaseClass), ClassReferenceOrNull(Typed.DefaultValue)});
		return true;
	}
	if (KeyType.GetClass() == UBlackboardKeyType_Enum::StaticClass())
	{
		const UBlackboardKeyType_Enum& Typed = static_cast<const UBlackboardKeyType_Enum&>(KeyType);
		if (Typed.EnumType == nullptr) return false;
		const int32 Index = Typed.EnumType->GetIndexByValue(Typed.DefaultValue);
		if (Index == INDEX_NONE) return false;
		const FString Enumerator = Typed.EnumType->GetNameStringByValue(Typed.DefaultValue);
		if (Enumerator.IsEmpty() || Enumerator == TEXT("MAX") || Enumerator.EndsWith(TEXT("_MAX")))
		{
			return false;
		}
		const FString EnumTypePath = Typed.EnumType->GetPathName();
		const bool bNativeType = EnumTypePath.StartsWith(TEXT("/Script/"));
		if ((Typed.EnumName.IsEmpty() && bNativeType)
			|| (!Typed.EnumName.IsEmpty() && (!bNativeType || Typed.EnumName != EnumTypePath)))
		{
			return false;
		}
		OutProperties = ObjectValue({TEXT("DefaultValue"), TEXT("EnumName"), TEXT("EnumType")},
			{StringValue(Enumerator), StringValue(Typed.EnumName), AssetReferenceValue(*Typed.EnumType)});
		return true;
	}
	if (KeyType.IsA<UBlackboardKeyType_Object>() || KeyType.IsA<UBlackboardKeyType_Class>()
		|| KeyType.IsA<UBlackboardKeyType_Enum>()) return false;
	bHandled = false;
	return true;
}

bool ReadEntryReadbackLeaf(const IPropertyHandle& Handle, UBlackboardKeyType& KeyType,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	const FProperty* Property = Handle.GetProperty();
	if (Property == nullptr) return false;
	if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
	{
		UObject* Value = nullptr;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		if (Value == nullptr) { OutValue.Kind = EAssetDocumentRuntimeValueKind::Null; return true; }
		const UClass* Class = Cast<UClass>(Value);
		if (Class == nullptr) return false;
		OutValue = ClassReferenceValue(*Class); return true;
	}
	if (CastField<FObjectPropertyBase>(Property) != nullptr)
	{
		UObject* Value = nullptr;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		if (Value == nullptr) { OutValue.Kind = EAssetDocumentRuntimeValueKind::Null; return true; }
		if (const UClass* Class = Cast<UClass>(Value)) OutValue = ClassReferenceValue(*Class);
		else OutValue = AssetReferenceValue(*Value);
		return true;
	}
	if (KeyType.IsA<UBlackboardKeyType_Enum>() && Property->GetName() == TEXT("DefaultValue"))
	{
		const UBlackboardKeyType_Enum& EnumKey = static_cast<const UBlackboardKeyType_Enum&>(KeyType);
		if (EnumKey.EnumType == nullptr) return false;
		const FString Name = EnumKey.EnumType->GetNameStringByValue(EnumKey.DefaultValue);
		if (Name.IsEmpty()) return false;
		OutValue = StringValue(Name); return true;
	}
	if (KeyType.IsA<UBlackboardKeyType_Enum>() && Property->GetName() == TEXT("EnumName"))
	{
		const UBlackboardKeyType_Enum& EnumKey = static_cast<const UBlackboardKeyType_Enum&>(KeyType);
		if (EnumKey.EnumType == nullptr || EnumKey.EnumName != EnumKey.EnumType->GetPathName()) return false;
		OutValue = StringValue(EnumKey.EnumType->GetPathName()); return true;
	}
	FString IgnoredKind;
	return ReadLeaf(Handle, OutValue, IgnoredKind);
}

bool CollectEntryReadbackProperties(
	UBlackboardKeyType& ExactKeyType,
	FAssetDocumentRuntimeValueDto& OutProperties)
{
	const UClass* KeyTypeClass = ExactKeyType.GetClass();
	if (KeyTypeClass == nullptr || KeyTypeClass->HasAnyClassFlags(
		CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		|| ExactKeyType.IsA<UBlackboardKeyType_NativeEnum>()) return false;
	if (!IsEditorPropertyContext(&ExactKeyType)) return false;
	if (ExactKeyType.GetClass() == UBlackboardKeyType_Struct::StaticClass())
	{
		return AssetDocumentAIBlackboardMutation::
			BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(ExactKeyType, OutProperties);
	}
	bool bStockHandled = false;
	if (!CollectStockEntryReadbackProperties(ExactKeyType, OutProperties, bStockHandled)) return false;
	if (bStockHandled) return true;
	FPropertyEditorModule* const PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr) return false;
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = false;
	TSharedPtr<IPropertyRowGenerator> Rows = PropertyEditor->CreatePropertyRowGenerator(Args);
	Rows->SetObjects({&ExactKeyType});

	TMap<FString, TSharedRef<IPropertyHandle>> Handles;
	if (!GatherTopLevelHandles(Rows->GetRootTreeNodes(), ExactKeyType, Handles, true)) return false;
	TArray<FString> Names;
	Handles.GetKeys(Names);
	Names.Sort();
	TArray<FString> PropertyKeys;
	TArray<FAssetDocumentRuntimeValueDto> PropertyValues;
	for (const FString& Name : Names)
	{
		FAssetDocumentRuntimeValueDto Value;
		if (!ReadEntryReadbackLeaf(*Handles.FindChecked(Name), ExactKeyType, Value))
		{
			return false;
		}
		PropertyKeys.Add(Name);
		PropertyValues.Add(MoveTemp(Value));
	}
	OutProperties = ObjectValue(MoveTemp(PropertyKeys), MoveTemp(PropertyValues));
	Handles.Reset();
	Rows.Reset();
	return true;
}

struct FEntryReadback
{
	const FBlackboardEntry* Entry = nullptr;
	UBlackboardKeyType* KeyType = nullptr;
};

bool ResolveEntryReadback(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FEntryReadback& OutReadback,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	FAssetDocumentAIBlackboardEntryReadbackRequest Request;
	if (!ParseAssetDocumentAIBlackboardEntryReadbackRequest(Invocation, Request))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The Blackboard entry readback request is not canonical."));
		return false;
	}
	UBlackboardData* const Blackboard = ResolveExactBlackboard(Request.BlackboardPath);
	if (Blackboard == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.EntryReadbackUnavailable"),
			TEXT("The requested Blackboard is unavailable for exact readback."));
		return false;
	}
	if (!IsEditorPropertyContext(Blackboard))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard entry readback requires the Editor game thread."));
		return false;
	}
	const FName KeyName(*Request.KeyName);
	const FBlackboard::FKey KeyId = Blackboard->GetKeyID(KeyName);
	const FBlackboardEntry* const VisibleEntry = Blackboard->GetKey(KeyId);
	const FBlackboardEntry* const LocalEntry = FindOwnedLocalEntry(*Blackboard, KeyName);
	if (LocalEntry == nullptr)
	{
		OutFailure = VisibleEntry != nullptr
			? Failure(TEXT("AIProvider.Blackboard.KeyInherited"),
				TEXT("The requested Blackboard key is inherited rather than owned locally."))
			: Failure(TEXT("AIProvider.Blackboard.KeyMissing"),
				TEXT("The requested Blackboard key is unavailable."));
		return false;
	}
	if (VisibleEntry != LocalEntry || LocalEntry->KeyType == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The requested owned Blackboard entry has no exact live key-type identity."));
		return false;
	}
	OutReadback.Entry = LocalEntry;
	OutReadback.KeyType = LocalEntry->KeyType.Get();
	return true;
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardKeyTypeInspect(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardInspectionLocal;
	FKeyTypeInspection Inspection;
	FAssetDocumentPrimitiveOutcome Outcome;
	if (!ResolveKeyTypeInspection(Invocation, Inspection, Outcome)) return Outcome;
	FAssetDocumentRuntimeValueDto Properties;
	FAssetDocumentRuntimeValueDto Schema;
	if (!CollectKeyTypeEvidence(Inspection, Properties, Schema, Outcome)) return Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = ObjectValue(
		{TEXT("canonical_properties"), TEXT("class_path"), TEXT("property_schema")},
		{MoveTemp(Properties), StringValue(Inspection.KeyType->GetClass()->GetPathName()),
			MoveTemp(Schema)});
	return Outcome;
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardEntryReadback(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver&,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardInspectionLocal;
	if (ActiveTransaction != nullptr)
	{
		return Failure(TEXT("AIProvider.InvalidTransaction"),
			TEXT("Blackboard entry readback does not admit an active transaction."));
	}
	FEntryReadback Readback;
	FAssetDocumentPrimitiveOutcome Outcome;
	if (!ResolveEntryReadback(Invocation, Readback, Outcome)) return Outcome;
	FAssetDocumentRuntimeValueDto Properties;
	if (!CollectEntryReadbackProperties(*Readback.KeyType, Properties))
	{
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The public PropertyEditor readback authority is unavailable."));
	}
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = ObjectValue(
		{TEXT("entry")},
		{ObjectValue(
			{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
				TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
			{StringValue(Readback.Entry->EntryCategory.IsNone()
					? FString() : Readback.Entry->EntryCategory.ToString()),
				StringValue(Readback.Entry->EntryDescription),
				ClassReferenceValue(*Readback.KeyType->GetClass()), MoveTemp(Properties),
				StringValue(Readback.Entry->EntryName.ToString()),
				BooleanValue(static_cast<bool>(Readback.Entry->bInstanceSynced))})});
	return Outcome;
}

namespace AIBlackboardInspectionLocal
{
TSharedPtr<IPropertyHandle> FindExactParentReadHandle(
	IPropertyRowGenerator& Rows, UBlackboardData& Blackboard)
{
	TArray<TSharedPtr<IPropertyHandle>> Matches;
	TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather =
		[&](const TArray<TSharedRef<IDetailTreeNode>>& Nodes)
	{
		for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
		{
			const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
			TArray<UObject*> Outers;
			if (Handle.IsValid()) Handle->GetOuterObjects(Outers);
			const FProperty* Property = Handle.IsValid() ? Handle->GetProperty() : nullptr;
			if (Property != nullptr && Property->GetFName() == GET_MEMBER_NAME_CHECKED(UBlackboardData, Parent)
				&& Property->GetOwnerClass() == UBlackboardData::StaticClass()
				&& Handle->GeneratePathToProperty() == TEXT("Parent")
				&& Outers.Num() == 1 && Outers[0] == &Blackboard) Matches.Add(Handle);
			TArray<TSharedRef<IDetailTreeNode>> Children; Node->GetChildren(Children, true); Gather(Children);
		}
	};
	Gather(Rows.GetRootTreeNodes());
	return Matches.Num() == 1 ? Matches[0] : nullptr;
}

bool ReadExactBlackboardParent(
	const FAssetDocumentAIBlackboardParentReadbackRequest& Request,
	const UObject*& OutParentObject,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	UBlackboardData* const Blackboard = ResolveExactBlackboard(Request.BlackboardPath);
	if (Blackboard == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ParentUnavailable"),
			TEXT("The requested Blackboard is unavailable for Parent readback."));
		return false;
	}
	if (!IsEditorPropertyContext(Blackboard))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard Parent readback requires the Editor game thread."));
		return false;
	}
	FPropertyEditorModule* const PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The public PropertyEditor authority is unavailable."));
		return false;
	}
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = false;
	const TSharedPtr<IPropertyRowGenerator> Rows = PropertyEditor->CreatePropertyRowGenerator(Args);
	if (!Rows.IsValid())
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The public PropertyEditor row generator is unavailable."));
		return false;
	}
	Rows->SetPropertyGenerationAllowListPaths({GET_MEMBER_NAME_CHECKED(UBlackboardData, Parent).ToString()});
	Rows->SetObjects({Blackboard});
	const TSharedPtr<IPropertyHandle> ParentHandle = FindExactParentReadHandle(*Rows, *Blackboard);
	if (!ParentHandle.IsValid())
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact public Parent property handle is unavailable."));
		return false;
	}
	if (ParentHandle->GetValue(OutParentObject) != FPropertyAccess::Success)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"),
			TEXT("The public Parent property readback failed."));
		return false;
	}
	return true;
}

FAssetDocumentPrimitiveOutcome MakeBlackboardParentReadbackOutcome(const UObject* ParentObject)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	if (ParentObject == nullptr)
	{
		Outcome.Value = ObjectValue({TEXT("parent_path_or_null")}, {NullValue()});
		return Outcome;
	}
	if (ParentObject->GetClass() != UBlackboardData::StaticClass() || ParentObject->GetPathName().IsEmpty())
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentClassMismatch"),
			TEXT("The public Parent property does not contain an exact Blackboard."));
	}
	Outcome.Value = ObjectValue({TEXT("parent_path_or_null")}, {StringValue(ParentObject->GetPathName())});
	return Outcome;
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardParentReadback(
	const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver&,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardInspectionLocal;
	if (ActiveTransaction != nullptr) return Failure(TEXT("AIProvider.InvalidTransaction"),
		TEXT("Blackboard Parent readback does not admit an active transaction."));
	FAssetDocumentAIBlackboardParentReadbackRequest Request;
	if (!ParseAssetDocumentAIBlackboardParentReadbackRequest(Invocation, Request)) return Failure(
		TEXT("AIProvider.InvalidValueShape"), TEXT("The Blackboard Parent readback request is not canonical."));
	const UObject* ParentObject = nullptr;
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	if (!ReadExactBlackboardParent(Request, ParentObject, FailureOutcome)) return FailureOutcome;
	return MakeBlackboardParentReadbackOutcome(ParentObject);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardLocalKeysReadback(
	const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver&,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardInspectionLocal;
	if (ActiveTransaction != nullptr) return Failure(TEXT("AIProvider.InvalidTransaction"),
		TEXT("Blackboard local-keys readback does not admit an active transaction."));
	FAssetDocumentAIBlackboardLocalKeysReadbackRequest Request;
	if (!ParseAssetDocumentAIBlackboardLocalKeysReadbackRequest(Invocation, Request)) return Failure(
		TEXT("AIProvider.InvalidValueShape"), TEXT("The Blackboard local-keys readback request is not canonical."));
	UBlackboardData* const Blackboard = ResolveExactBlackboard(Request.BlackboardPath);
	if (Blackboard == nullptr) return Failure(TEXT("AIProvider.Blackboard.LocalKeysReadbackUnavailable"),
		TEXT("The requested Blackboard is unavailable for local-keys readback."));
	if (!IsEditorPropertyContext(Blackboard)) return Failure(TEXT("AIProvider.InvalidContext"),
		TEXT("Blackboard local-keys readback requires the Editor game thread."));
	TSet<FName> SeenNames;
	TArray<FAssetDocumentRuntimeValueDto> Names;
	for (const FBlackboardEntry& Entry : Blackboard->GetKeys())
	{
		if (Entry.EntryName.IsNone()) return Failure(TEXT("AIProvider.Blackboard.LocalKeyNameInvalid"),
			TEXT("A Blackboard local key has no authored name."));
		if (SeenNames.Contains(Entry.EntryName)) return Failure(TEXT("AIProvider.Blackboard.DuplicateKey"),
			TEXT("Blackboard local key names must be unique."));
		SeenNames.Add(Entry.EntryName);
		Names.Add(StringValue(Entry.EntryName.ToString()));
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = ObjectValue({TEXT("ordered_local_key_names")}, {ArrayValue(MoveTemp(Names))});
	return Outcome;
}

namespace AIBlackboardInspectionLocal
{
TSharedPtr<IPropertyHandle> FindInspectionKeysHandle(
	IPropertyRowGenerator& Rows,
	UBlackboardData& Blackboard)
{
	TArray<TSharedPtr<IPropertyHandle>> Matches;
	TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather =
		[&](const TArray<TSharedRef<IDetailTreeNode>>& Nodes)
	{
		for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
		{
			const TSharedPtr<IPropertyHandle> Candidate = Node->CreatePropertyHandle();
			TArray<UObject*> Outers;
			if (Candidate.IsValid()) Candidate->GetOuterObjects(Outers);
			const FProperty* Property = Candidate.IsValid() ? Candidate->GetProperty() : nullptr;
			if (Property != nullptr
				&& Property->GetFName() == GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys)
				&& Property->GetOwnerClass() == UBlackboardData::StaticClass()
				&& Candidate->GeneratePathToProperty() == TEXT("Keys")
				&& Outers.Num() == 1 && Outers[0] == &Blackboard)
			{
				Matches.Add(Candidate);
			}
			TArray<TSharedRef<IDetailTreeNode>> Children;
			Node->GetChildren(Children, true);
			Gather(Children);
		}
	};
	Gather(Rows.GetRootTreeNodes());
	return Matches.Num() == 1 ? Matches[0] : nullptr;
}
} // namespace

bool AssetDocumentAIBlackboardMutation::BuildAssetDocumentAIBlackboardEntryMetadataReadback(
	IPropertyRowGenerator& BlackboardRows,
	UBlackboardData& Blackboard,
	const FString& KeyName,
	UBlackboardKeyType& KeyType,
	const FAssetDocumentRuntimeValueDto& CanonicalProperties,
	FAssetDocumentRuntimeValueDto& OutEntry,
	TArray<FString>& OutLocalKeyNames)
{
	using namespace AIBlackboardInspectionLocal;
	if (KeyName.IsEmpty() || KeyType.GetOuter() != &Blackboard
		|| CanonicalProperties.Kind != EAssetDocumentRuntimeValueKind::Object) return false;
	const TSharedPtr<IPropertyHandle> Keys = FindInspectionKeysHandle(BlackboardRows, Blackboard);
	if (!Keys.IsValid()) return false;
	uint32 ChildCount = 0;
	if (Keys->GetNumChildren(ChildCount) != FPropertyAccess::Success) return false;
	OutLocalKeyNames.Reset();
	bool bFound = false;
	for (uint32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
	{
		const TSharedPtr<IPropertyHandle> Entry = Keys->GetChildHandle(ChildIndex);
		const TSharedPtr<IPropertyHandle> Name = Entry.IsValid() ? Entry->GetChildHandle(TEXT("EntryName")) : nullptr;
		const TSharedPtr<IPropertyHandle> EntryKeyType = Entry.IsValid() ? Entry->GetChildHandle(TEXT("KeyType")) : nullptr;
		if (!Name.IsValid() || !EntryKeyType.IsValid()) return false;
		FName LocalName;
		UObject* LocalKeyTypeObject = nullptr;
		if (Name->GetValue(LocalName) != FPropertyAccess::Success
			|| EntryKeyType->GetValue(LocalKeyTypeObject) != FPropertyAccess::Success) return false;
		UBlackboardKeyType* const LocalKeyType = Cast<UBlackboardKeyType>(LocalKeyTypeObject);
		if (LocalKeyType == nullptr || LocalKeyType->GetOuter() != &Blackboard) continue;
		OutLocalKeyNames.Add(LocalName.ToString());
		if (LocalName.ToString() != KeyName || LocalKeyType != &KeyType) continue;
		if (bFound) return false;
		const TSharedPtr<IPropertyHandle> Description = Entry->GetChildHandle(TEXT("EntryDescription"));
		const TSharedPtr<IPropertyHandle> Category = Entry->GetChildHandle(TEXT("EntryCategory"));
		const TSharedPtr<IPropertyHandle> InstanceSynced = Entry->GetChildHandle(TEXT("bInstanceSynced"));
		if (!Description.IsValid() || !Category.IsValid() || !InstanceSynced.IsValid()) return false;
		FString DescriptionValue;
		FName CategoryValue;
		bool InstanceSyncedValue = false;
		if (Description->GetValue(DescriptionValue) != FPropertyAccess::Success
			|| Category->GetValue(CategoryValue) != FPropertyAccess::Success
			|| InstanceSynced->GetValue(InstanceSyncedValue) != FPropertyAccess::Success) return false;
			OutEntry = ObjectValue(
			{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
				TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
			{StringValue(CategoryValue.IsNone() ? FString() : CategoryValue.ToString()),
					StringValue(DescriptionValue), ClassReferenceValue(*KeyType.GetClass()), CanonicalProperties,
				StringValue(LocalName.ToString()), BooleanValue(InstanceSyncedValue)});
		bFound = true;
	}
	return bFound;
}

bool AssetDocumentAIBlackboardMutation::BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(
	UBlackboardKeyType& KeyType, FAssetDocumentRuntimeValueDto& OutProperties)
{
	using namespace AIBlackboardInspectionLocal;
	if (KeyType.GetClass() == UBlackboardKeyType_Struct::StaticClass())
	{
		UBlackboardKeyType_Struct& StructKey = static_cast<UBlackboardKeyType_Struct&>(KeyType);
		const AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::FReadResult Read =
			AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::ReadCanonicalValue(
				StructKey.DefaultValue,
				AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::ESource::ExactLiveDefaultValue,
				&StructKey);
		if (!Read.bSucceeded)
		{
			return false;
		}
		OutProperties = ObjectValue({TEXT("DefaultValue")}, {Read.Value});
		return IsAssetDocumentAICanonicalRuntimeValue(OutProperties);
	}
	return CollectEntryReadbackProperties(KeyType, OutProperties);
}

bool AssetDocumentAIBlackboardMutation::BuildAssetDocumentAIBlackboardCanonicalKeyTypePropertiesFromRows(
	UBlackboardKeyType& KeyType,
	IPropertyRowGenerator& Rows,
	FAssetDocumentRuntimeValueDto& OutProperties)
{
	using namespace AIBlackboardInspectionLocal;
	const UClass* KeyTypeClass = KeyType.GetClass();
	if (KeyTypeClass == nullptr || KeyTypeClass->HasAnyClassFlags(
		CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		|| KeyType.IsA<UBlackboardKeyType_NativeEnum>()
		|| KeyType.IsA<UBlackboardKeyType_Object>()
		|| KeyType.IsA<UBlackboardKeyType_Class>()
		|| KeyType.IsA<UBlackboardKeyType_Enum>()) return false;
	TMap<FString, TSharedRef<IPropertyHandle>> Handles;
	if (!GatherTopLevelHandles(Rows.GetRootTreeNodes(), KeyType, Handles, true)) return false;
	TArray<FString> Names;
	Handles.GetKeys(Names);
	Names.Sort();
	TArray<FAssetDocumentRuntimeValueDto> Values;
	for (const FString& Name : Names)
	{
		FAssetDocumentRuntimeValueDto Value;
		if (!ReadEntryReadbackLeaf(*Handles.FindChecked(Name), KeyType, Value)) return false;
		Values.Add(MoveTemp(Value));
	}
	OutProperties = ObjectValue(MoveTemp(Names), MoveTemp(Values));
	return IsAssetDocumentAICanonicalRuntimeValue(OutProperties);
}

bool AssetDocumentAIBlackboardMutation::IsAssetDocumentAIBlackboardDirectValueKeyType(const UClass* KeyTypeClass)
{
	using namespace AIBlackboardInspectionLocal;
	return !DirectValueComponentNames(KeyTypeClass).IsEmpty();
}

bool AssetDocumentAIBlackboardMutation::WriteAssetDocumentAIBlackboardDirectValueKeyType(
	UBlackboardKeyType& KeyType, const FAssetDocumentRuntimeValueDto& Properties)
{
	using namespace AIBlackboardInspectionLocal;
	const TArray<FString> Names = DirectValueComponentNames(KeyType.GetClass());
	if (Names.IsEmpty() || Properties.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Properties.ObjectKeys != TArray<FString>{TEXT("DefaultValue")}
		|| Properties.Elements.Num() != 1) return false;
	const FAssetDocumentRuntimeValueDto& DefaultValue = Properties.Elements[0];
	const bool bUseDefaultValue = DefaultValue.Kind != EAssetDocumentRuntimeValueKind::Null;
	double Components[3] = {0.0, 0.0, 0.0};
	if (bUseDefaultValue)
	{
		if (DefaultValue.Kind != EAssetDocumentRuntimeValueKind::Object
			|| DefaultValue.ObjectKeys != Names
			|| DefaultValue.Elements.Num() != Names.Num()) return false;
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			const FAssetDocumentRuntimeValueDto& Component = DefaultValue.Elements[Index];
			if (Component.Kind == EAssetDocumentRuntimeValueKind::Number)
			{
				Components[Index] = Component.NumberValue;
			}
			else if (Component.Kind == EAssetDocumentRuntimeValueKind::Integer)
			{
				Components[Index] = static_cast<double>(Component.IntegerValue);
			}
			else
			{
				return false;
			}
			if (!FMath::IsFinite(Components[Index])) return false;
		}
	}
	if (UBlackboardKeyType_Vector* Vector = Cast<UBlackboardKeyType_Vector>(&KeyType))
	{
		Vector->DefaultValue = FVector(Components[0], Components[1], Components[2]);
		Vector->bUseDefaultValue = bUseDefaultValue;
		return true;
	}
	if (UBlackboardKeyType_Rotator* Rotator = Cast<UBlackboardKeyType_Rotator>(&KeyType))
	{
		// FRotator(Pitch, Yaw, Roll); components are ordered Pitch, Roll, Yaw.
		Rotator->DefaultValue = FRotator(Components[0], Components[2], Components[1]);
		Rotator->bUseDefaultValue = bUseDefaultValue;
		return true;
	}
	return false;
}
