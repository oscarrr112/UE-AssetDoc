// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "IDetailCustomization.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentAIBlackboardMutation
{
namespace
{
#if WITH_DEV_AUTOMATION_TESTS
EAssetDocumentAIBlackboardTestFailurePoint GBlackboardTestFailurePoint =
	EAssetDocumentAIBlackboardTestFailurePoint::None;
EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint GBlackboardMetadataWriteTestFailurePoint =
	EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::None;
int32 GBlackboardMetadataWriteTestKeyTypeLeafIndex = INDEX_NONE;
bool GBlackboardMetadataGeneratorAuditActive = false;
TArray<FString> GBlackboardMetadataGeneratorAudit;
#endif

class FAssetDocumentAICanonicalStockKeyTypeDetails final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FAssetDocumentAICanonicalStockKeyTypeDetails>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder&) override
	{
	}
};

bool TryMakeCanonicalStockRows(
	UBlackboardKeyType& KeyType,
	const TSet<FString>& PropertyAllowList,
	TSharedPtr<IPropertyRowGenerator>& OutRows)
{
	if (!IsEditorPropertyContext(&KeyType)) return false;
	FPropertyEditorModule* PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr) return false;
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = false;
	OutRows = PropertyEditor->CreatePropertyRowGenerator(Args);
	OutRows->OnRowsRefreshed().AddLambda([]() {});
	OutRows->RegisterInstancedCustomPropertyLayout(
		KeyType.GetClass(),
		FOnGetDetailCustomizationInstance::CreateStatic(
			&FAssetDocumentAICanonicalStockKeyTypeDetails::MakeInstance));
	OutRows->SetPropertyGenerationAllowListPaths(PropertyAllowList);
	OutRows->SetObjects({&KeyType});
	#if WITH_DEV_AUTOMATION_TESTS
	RecordBlackboardMetadataGenerator(KeyType, &PropertyAllowList);
	#endif
	return true;
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

bool IsCanonicalPropertyObject(const FAssetDocumentRuntimeValueDto& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Object
		&& IsAssetDocumentAICanonicalRuntimeValue(Value);
}

bool ResolveCanonicalReference(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* ExpectedKind,
	UObject*& OutObject)
{
	OutObject = nullptr;
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Null) return true;
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys != TArray<FString>{TEXT("Kind"), TEXT("Path")}
		|| Value.Elements.Num() != 2
		|| Value.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
		|| Value.Elements[0].StringValue != ExpectedKind
		|| Value.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String
		|| Value.Elements[1].StringValue.IsEmpty()) return false;
	const FSoftObjectPath Path(Value.Elements[1].StringValue);
	OutObject = Path.ResolveObject();
	if (OutObject == nullptr) OutObject = Path.TryLoad();
	return OutObject != nullptr && OutObject->GetPathName() == Value.Elements[1].StringValue;
}

bool ResolveEnumValue(
	const UEnum& EnumType,
	const FAssetDocumentRuntimeValueDto& Value,
	uint8& OutValue)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String) return false;
	for (int32 Index = 0; Index < EnumType.NumEnums(); ++Index)
	{
		const FString Name = EnumType.GetNameStringByIndex(Index);
		const int64 Candidate = EnumType.GetValueByIndex(Index);
		if (Name == Value.StringValue && Candidate >= 0 && Candidate <= MAX_uint8
			&& Name != TEXT("MAX") && !Name.EndsWith(TEXT("_MAX")))
		{
			OutValue = static_cast<uint8>(Candidate);
			return true;
		}
	}
	return false;
}

bool ValidateStockCanonicalProperties(
	UBlackboardKeyType& KeyType,
	const FAssetDocumentRuntimeValueDto& Properties,
	UEnum*& OutCanonicalEnumType)
{
	OutCanonicalEnumType = nullptr;
	if (KeyType.GetClass() == UBlackboardKeyType_Object::StaticClass())
	{
		if (Properties.ObjectKeys != TArray<FString>{TEXT("BaseClass"), TEXT("DefaultValue")}
			|| Properties.Elements.Num() != 2) return false;
		UObject* BaseClassObject = nullptr;
		UObject* DefaultValue = nullptr;
		if (!ResolveCanonicalReference(Properties.Elements[0], TEXT("ClassRef"), BaseClassObject)
			|| !ResolveCanonicalReference(Properties.Elements[1], TEXT("AssetRef"), DefaultValue)) return false;
		const UClass* BaseClass = Cast<UClass>(BaseClassObject);
		return BaseClass != nullptr && (DefaultValue == nullptr || DefaultValue->IsA(BaseClass));
	}
	if (KeyType.GetClass() == UBlackboardKeyType_Class::StaticClass())
	{
		if (Properties.ObjectKeys != TArray<FString>{TEXT("BaseClass"), TEXT("DefaultValue")}
			|| Properties.Elements.Num() != 2) return false;
		UObject* BaseClassObject = nullptr;
		UObject* DefaultValueObject = nullptr;
		if (!ResolveCanonicalReference(Properties.Elements[0], TEXT("ClassRef"), BaseClassObject)
			|| !ResolveCanonicalReference(Properties.Elements[1], TEXT("ClassRef"), DefaultValueObject)) return false;
		const UClass* BaseClass = Cast<UClass>(BaseClassObject);
		const UClass* DefaultValue = Cast<UClass>(DefaultValueObject);
		return BaseClass != nullptr && (DefaultValue == nullptr || DefaultValue->IsChildOf(BaseClass));
	}
	if (KeyType.GetClass() == UBlackboardKeyType_Enum::StaticClass())
	{
		if (Properties.ObjectKeys != TArray<FString>{TEXT("DefaultValue"), TEXT("EnumName"), TEXT("EnumType")}
			|| Properties.Elements.Num() != 3
			|| Properties.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String) return false;
		UObject* EnumTypeObject = nullptr;
		if (!ResolveCanonicalReference(Properties.Elements[2], TEXT("AssetRef"), EnumTypeObject)) return false;
		OutCanonicalEnumType = Cast<UEnum>(EnumTypeObject);
		if (OutCanonicalEnumType == nullptr) return false;
		const FString EnumPath = OutCanonicalEnumType->GetPathName();
		const bool bNative = EnumPath.StartsWith(TEXT("/Script/"));
		if ((bNative && Properties.Elements[1].StringValue != EnumPath)
			|| (!bNative && !Properties.Elements[1].StringValue.IsEmpty())) return false;
		uint8 IgnoredValue = 0;
		return ResolveEnumValue(*OutCanonicalEnumType, Properties.Elements[0], IgnoredValue);
	}
	return true;
}

bool CanWriteLeaf(
	const IPropertyHandle& Handle,
	const FAssetDocumentRuntimeValueDto& Value,
	const UBlackboardKeyType& KeyType,
	const UEnum* CanonicalEnumType = nullptr)
{
	const FProperty* Property = Handle.GetProperty();
	if (Property == nullptr) return false;
	if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
	{
		UObject* Object = nullptr;
		return ResolveCanonicalReference(Value, TEXT("ClassRef"), Object)
			&& (Object == nullptr || (Cast<UClass>(Object) != nullptr
				&& Cast<UClass>(Object)->IsChildOf(ClassProperty->MetaClass)));
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		UObject* Object = nullptr;
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Null) return true;
		if (!ResolveCanonicalReference(Value,
			Value.Elements.IsValidIndex(0) && Value.Elements[0].StringValue == TEXT("ClassRef")
				? TEXT("ClassRef") : TEXT("AssetRef"), Object)) return false;
		return Object != nullptr && Object->IsA(ObjectProperty->PropertyClass);
	}
	if (KeyType.GetClass() == UBlackboardKeyType_Enum::StaticClass()
		&& Property->GetFName() == TEXT("DefaultValue"))
	{
		uint8 EnumValue = 0;
		const UEnum* EnumType = CanonicalEnumType != nullptr
			? CanonicalEnumType : static_cast<const UBlackboardKeyType_Enum&>(KeyType).EnumType.Get();
		return EnumType != nullptr && ResolveEnumValue(*EnumType, Value, EnumValue);
	}
	if (CastField<FBoolProperty>(Property) != nullptr)
		return Value.Kind == EAssetDocumentRuntimeValueKind::Boolean;
	if (CastField<FIntProperty>(Property) != nullptr)
		return Value.Kind == EAssetDocumentRuntimeValueKind::Integer
			&& Value.IntegerValue >= MIN_int32 && Value.IntegerValue <= MAX_int32;
	if (CastField<FInt64Property>(Property) != nullptr)
		return Value.Kind == EAssetDocumentRuntimeValueKind::Integer;
	if (CastField<FByteProperty>(Property) != nullptr)
		return Value.Kind == EAssetDocumentRuntimeValueKind::Integer
			&& Value.IntegerValue >= 0 && Value.IntegerValue <= MAX_uint8;
	if (CastField<FFloatProperty>(Property) != nullptr || CastField<FDoubleProperty>(Property) != nullptr)
		return Value.Kind == EAssetDocumentRuntimeValueKind::Number && FMath::IsFinite(Value.NumberValue);
	if (CastField<FStrProperty>(Property) != nullptr || CastField<FNameProperty>(Property) != nullptr)
		return Value.Kind == EAssetDocumentRuntimeValueKind::String;
	return false;
}

bool ReadLeaf(const IPropertyHandle& Handle, FAssetDocumentRuntimeValueDto& OutValue)
{
	const FProperty* Property = Handle.GetProperty();
	if (CastField<FBoolProperty>(Property) != nullptr)
	{
		bool Value = false;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Boolean;
		OutValue.BooleanValue = Value;
		return true;
	}
	if (CastField<FIntProperty>(Property) != nullptr)
	{
		int32 Value = 0;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Integer;
		OutValue.IntegerValue = Value;
		return true;
	}
	if (CastField<FInt64Property>(Property) != nullptr)
	{
		int64 Value = 0;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Integer;
		OutValue.IntegerValue = Value;
		return true;
	}
	if (CastField<FByteProperty>(Property) != nullptr)
	{
		uint8 Value = 0;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Integer;
		OutValue.IntegerValue = Value;
		return true;
	}
	if (CastField<FFloatProperty>(Property) != nullptr)
	{
		float Value = 0.0f;
		if (Handle.GetValue(Value) != FPropertyAccess::Success || !FMath::IsFinite(Value)) return false;
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Number;
		OutValue.NumberValue = Value;
		return true;
	}
	if (CastField<FDoubleProperty>(Property) != nullptr)
	{
		double Value = 0.0;
		if (Handle.GetValue(Value) != FPropertyAccess::Success || !FMath::IsFinite(Value)) return false;
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Number;
		OutValue.NumberValue = Value;
		return true;
	}
	if (CastField<FStrProperty>(Property) != nullptr)
	{
		FString Value;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = StringValue(MoveTemp(Value));
		return true;
	}
	if (CastField<FNameProperty>(Property) != nullptr)
	{
		FName Value;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = StringValue(Value.ToString());
		return true;
	}
	return false;
}

void GatherTopLevelHandles(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	UObject& Owner,
	TMap<FString, TSharedRef<IPropertyHandle>>& OutHandles)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid() && Handle->GetProperty() != nullptr
			&& IsAdmittedLeaf(*Handle->GetProperty(), *Handle))
		{
			TArray<UObject*> Outers;
			Handle->GetOuterObjects(Outers);
			if (Outers.Num() == 1 && Outers[0] == &Owner)
			{
				OutHandles.Add(Handle->GetProperty()->GetName(), Handle.ToSharedRef());
			}
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		GatherTopLevelHandles(Children, Owner, OutHandles);
	}
}
} // namespace

FAssetDocumentPrimitiveOutcome Failure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	return MakeAssetDocumentAIPrimitiveFailure(
		EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message, Diagnostics);
}

template <typename TApply>
FAssetDocumentPrimitiveOutcome ApplyWithCurrentAssetIdentity(
	const IAssetDocumentVerifiedHandleLease& Lease,
	TApply&& Apply)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult IdentityResult =
		WithAssetDocumentEditorAssetIdentity(
			Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
			{
				Outcome = Apply(Identity);
				return FAssetDocumentContractValidationResult{
					Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded, {}};
			});
	return !IdentityResult.bValid && Outcome.Diagnostics.IsEmpty()
		? Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The mutable Asset lease has no current Editor identity."))
		: Outcome;
}

FAssetDocumentPrimitiveOutcome FailureFromPreimage(
	const FAssetDocumentEditorTypedPreimageResult& Result)
{
	return Failure(TEXT("AIProvider.OperationRejected"),
		TEXT("The admitted Blackboard typed-preimage action was rejected."), Result.Diagnostics);
}

FAssetDocumentPrimitiveOutcome FailureFromValidation(
	const FAssetDocumentContractValidationResult& Result)
{
	return Failure(TEXT("AIProvider.OperationRejected"),
		TEXT("The active transaction rejected the complete Blackboard footprint."), Result.Diagnostics);
}

FAssetDocumentPrimitiveOutcome ParentSetFailureWithLowerDetail(
	const TCHAR* Code,
	const TCHAR* DefaultMessage,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> LowerDiagnostics)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = DefaultMessage;
	if (!LowerDiagnostics.IsEmpty())
	{
		if (!LowerDiagnostics[0].Message.IsEmpty())
		{
			Diagnostic.Message = LowerDiagnostics[0].Message;
		}
		Diagnostic.Context = LowerDiagnostics[0].Context;
	}
	return Failure(Code, DefaultMessage, {Diagnostic});
}

FAssetDocumentPrimitiveOutcome ParentSetFailureFromPreimage(
	const FAssetDocumentEditorTypedPreimageResult& Result)
{
	return ParentSetFailureWithLowerDetail(TEXT("AIProvider.OperationRejected"),
		TEXT("The admitted Blackboard typed-preimage action was rejected."), Result.Diagnostics);
}

FAssetDocumentPrimitiveOutcome NewKeyAuthorityFailure(const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/ordered_keys");
	Diagnostic.Code = TEXT("AIProvider.Blackboard.NewKeyTypeRequiresBlockedAuthority");
	Diagnostic.Message = Message;
	FAssetDocumentRuntimeDiagnosticContext& Context = Diagnostic.Context.AddDefaulted_GetRef();
	Context.Key = TEXT("blocker");
	Context.Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Context.Value.StringValue = TEXT("AD.G1.BLOCK.AI.BLACKBOARD_KEY_TYPE_AUTHORITY");
	return Failure(TEXT("AIProvider.Blackboard.NewKeyTypeRequiresBlockedAuthority"), Message, {Diagnostic});
}

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto ArrayValue(TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

UBlackboardData* FindBlackboard(const FString& Path)
{
	UBlackboardData* Blackboard = Cast<UBlackboardData>(FSoftObjectPath(Path).ResolveObject());
	return Blackboard != nullptr && Blackboard->GetPathName() == Path ? Blackboard : nullptr;
}

UBlackboardKeyType* FindOwnedKeyType(UBlackboardData& Blackboard, const FString& Name)
{
	for (const FBlackboardEntry& Entry : Blackboard.Keys)
	{
		if (Entry.EntryName.ToString() == Name && Entry.KeyType != nullptr
			&& Entry.KeyType->GetOuter() == &Blackboard)
		{
			return Entry.KeyType.Get();
		}
	}
	return nullptr;
}

bool IsEditorPropertyContext(const UObject* Object)
{
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet() || IsGarbageCollecting())
	{
		return false;
	}
	if (Object == nullptr) return true;
	const UPackage* Package = Object->GetOutermost();
	return Package != nullptr
		&& !Object->HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed)
		&& !Object->HasAnyInternalFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable)
		&& !Package->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& !Package->HasAnyInternalFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable);
}

bool TryMakeRows(
	UObject& Object,
	TSharedPtr<IPropertyRowGenerator>& OutRows,
	const TSet<FString>* PropertyAllowList)
{
	if (!IsEditorPropertyContext(&Object)) return false;
	FPropertyEditorModule* PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr) return false;
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = false;
	OutRows = PropertyEditor->CreatePropertyRowGenerator(Args);
	OutRows->OnRowsRefreshed().AddLambda([]() {});
	#if WITH_DEV_AUTOMATION_TESTS
	if (PropertyAllowList != nullptr) RecordBlackboardMetadataGenerator(Object, PropertyAllowList);
	#endif
	if (PropertyAllowList != nullptr) OutRows->SetPropertyGenerationAllowListPaths(*PropertyAllowList);
	OutRows->SetObjects({&Object});
	return true;
}

TSharedPtr<IPropertyHandle> FindUniqueOwnedPropertyHandle(
	IPropertyRowGenerator& Rows,
	UObject& Owner,
	const FName PropertyName)
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
			FProperty* Property = Candidate.IsValid() ? Candidate->GetProperty() : nullptr;
			const FString StablePath = Candidate.IsValid() ? Candidate->GeneratePathToProperty() : FString();
			if (Property != nullptr && Property->GetFName() == PropertyName
				&& Property->GetOwnerClass() == UBlackboardData::StaticClass()
				&& StablePath == PropertyName.ToString() && Outers.Num() == 1 && Outers[0] == &Owner
				&& !Matches.ContainsByPredicate([&StablePath](const TSharedPtr<IPropertyHandle>& Existing)
					{ return Existing.IsValid() && Existing->GeneratePathToProperty() == StablePath; }))
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

bool BuildPropertyWrites(
	UBlackboardKeyType& KeyType,
	const FAssetDocumentRuntimeValueDto& Properties,
	TSharedPtr<IPropertyRowGenerator>& OutRows,
	TArray<TSharedRef<IPropertyHandle>>& OutHandles,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutValues)
{
	if (!IsCanonicalPropertyObject(Properties) || !TryMakeRows(KeyType, OutRows)) return false;
	TMap<FString, TSharedRef<IPropertyHandle>> Handles;
	GatherTopLevelHandles(OutRows->GetRootTreeNodes(), KeyType, Handles);
	for (int32 Index = 0; Index < Properties.ObjectKeys.Num(); ++Index)
	{
		const TSharedRef<IPropertyHandle>* Handle = Handles.Find(Properties.ObjectKeys[Index]);
		if (Handle == nullptr || !CanWriteLeaf(**Handle, Properties.Elements[Index], KeyType)) return false;
		OutHandles.Add(*Handle);
		OutValues.Add(&Properties.Elements[Index]);
	}
	return true;
}

bool BuildCompletePropertyWrites(
	UBlackboardKeyType& KeyType,
	const FAssetDocumentRuntimeValueDto& Properties,
	TSharedPtr<IPropertyRowGenerator>& OutRows,
	TArray<TSharedRef<IPropertyHandle>>& OutHandles,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutValues,
	UEnum*& OutCanonicalEnumType)
{
	const bool bStockExactCodec = KeyType.GetClass() == UBlackboardKeyType_Object::StaticClass()
		|| KeyType.GetClass() == UBlackboardKeyType_Class::StaticClass()
		|| KeyType.GetClass() == UBlackboardKeyType_Enum::StaticClass();
	if (!IsCanonicalPropertyObject(Properties)) return false;
	TSet<FString> PropertyAllowList;
	for (const FString& Name : Properties.ObjectKeys) PropertyAllowList.Add(Name);
	if (bStockExactCodec)
	{
		if (!TryMakeCanonicalStockRows(KeyType, PropertyAllowList, OutRows)) return false;
	}
	else if (!TryMakeRows(KeyType, OutRows)) return false;
	// The stock Object/Class/Enum key types have a fixed property set (ValidateStockCanonicalProperties checks it).
	// Their current state is not read here: a key type just staged has no valid canonical state yet (an Enum key
	// has no EnumType until it is written).
	if (!bStockExactCodec)
	{
		FAssetDocumentRuntimeValueDto AdmittedProperties;
		if (!BuildAssetDocumentAIBlackboardCanonicalKeyTypePropertiesFromRows(KeyType, *OutRows, AdmittedProperties)
			|| AdmittedProperties.ObjectKeys != Properties.ObjectKeys) return false;
		OutRows->SetPropertyGenerationAllowListPaths(PropertyAllowList);
		OutRows->SetObjects({&KeyType});
		#if WITH_DEV_AUTOMATION_TESTS
		RecordBlackboardMetadataGenerator(KeyType, &PropertyAllowList);
		#endif
	}
	if (!ValidateStockCanonicalProperties(KeyType, Properties, OutCanonicalEnumType)) return false;
	TMap<FString, TSharedRef<IPropertyHandle>> Handles;
	GatherTopLevelHandles(OutRows->GetRootTreeNodes(), KeyType, Handles);
	TArray<FString> Names;
	Handles.GetKeys(Names);
	Names.Sort();
	if (Properties.ObjectKeys != Names
		|| Properties.Elements.Num() != Properties.ObjectKeys.Num()) return false;
	for (int32 Index = 0; Index < Properties.ObjectKeys.Num(); ++Index)
	{
		const TSharedRef<IPropertyHandle>* Handle = Handles.Find(Properties.ObjectKeys[Index]);
		if (Handle == nullptr || !CanWriteLeaf(
			**Handle, Properties.Elements[Index], KeyType, OutCanonicalEnumType)) return false;
		OutHandles.Add(*Handle);
		OutValues.Add(&Properties.Elements[Index]);
	}
	return true;
}

bool WriteLeaf(
	IPropertyHandle& Handle,
	const FAssetDocumentRuntimeValueDto& Value,
	const UBlackboardKeyType* KeyType,
	const UEnum* CanonicalEnumType)
{
	FProperty* Property = Handle.GetProperty();
	if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
	{
		UObject* Object = nullptr;
		if (!ResolveCanonicalReference(Value, TEXT("ClassRef"), Object)) return false;
		UClass* Class = Cast<UClass>(Object);
		if (Class != nullptr && !Class->IsChildOf(ClassProperty->MetaClass)) return false;
		return Handle.SetValue(Class, EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		UObject* Object = nullptr;
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Null
			&& !ResolveCanonicalReference(Value,
				Value.Elements.IsValidIndex(0) && Value.Elements[0].StringValue == TEXT("ClassRef")
					? TEXT("ClassRef") : TEXT("AssetRef"), Object)) return false;
		if (Object != nullptr && !Object->IsA(ObjectProperty->PropertyClass)) return false;
		return Handle.SetValue(Object, EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	}
	if (KeyType != nullptr && KeyType->GetClass() == UBlackboardKeyType_Enum::StaticClass()
		&& Property != nullptr && Property->GetFName() == TEXT("DefaultValue"))
	{
		uint8 EnumValue = 0;
		const UEnum* EnumType = CanonicalEnumType != nullptr
			? CanonicalEnumType : static_cast<const UBlackboardKeyType_Enum&>(*KeyType).EnumType.Get();
		return EnumType != nullptr && ResolveEnumValue(*EnumType, Value, EnumValue)
			&& Handle.SetValue(EnumValue, EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	}
	if (CastField<FBoolProperty>(Property) != nullptr)
		return Handle.SetValue(Value.BooleanValue, EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	if (CastField<FIntProperty>(Property) != nullptr)
		return Handle.SetValue(static_cast<int32>(Value.IntegerValue), EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	if (CastField<FInt64Property>(Property) != nullptr)
		return Handle.SetValue(Value.IntegerValue, EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	if (CastField<FByteProperty>(Property) != nullptr)
		return Handle.SetValue(static_cast<uint8>(Value.IntegerValue), EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	if (CastField<FFloatProperty>(Property) != nullptr)
		return Handle.SetValue(static_cast<float>(Value.NumberValue), EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	if (CastField<FDoubleProperty>(Property) != nullptr)
		return Handle.SetValue(Value.NumberValue, EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	if (CastField<FStrProperty>(Property) != nullptr)
		return Handle.SetValue(Value.StringValue, EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	if (CastField<FNameProperty>(Property) != nullptr)
		return Handle.SetValue(FName(Value.StringValue), EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success;
	return false;
}

bool BuildCanonicalProperties(
	TConstArrayView<TSharedRef<IPropertyHandle>> Handles,
	FAssetDocumentRuntimeValueDto& OutProperties)
{
	TArray<TPair<FString, FAssetDocumentRuntimeValueDto>> Rows;
	for (const TSharedRef<IPropertyHandle>& Handle : Handles)
	{
		FAssetDocumentRuntimeValueDto Value;
		if (Handle->GetProperty() == nullptr || !ReadLeaf(*Handle, Value)) return false;
		Rows.Add({Handle->GetProperty()->GetName(), MoveTemp(Value)});
	}
	Rows.Sort([](const auto& Left, const auto& Right) { return Left.Key < Right.Key; });
	TArray<FString> Keys;
	TArray<FAssetDocumentRuntimeValueDto> Values;
	for (TPair<FString, FAssetDocumentRuntimeValueDto>& Row : Rows)
	{
		Keys.Add(MoveTemp(Row.Key));
		Values.Add(MoveTemp(Row.Value));
	}
	OutProperties = ObjectValue(MoveTemp(Keys), MoveTemp(Values));
	return IsAssetDocumentAICanonicalRuntimeValue(OutProperties);
}

FAssetDocumentMutationFootprint MakeFootprint(const FString& StableId, const TCHAR* ComponentId)
{
	FAssetDocumentMutationFootprint Result;
	Result.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	Result.StableId = StableId;
	Result.RollbackComponentId = ComponentId;
	return Result;
}

FAssetDocumentPrimitiveOutcome MakeSuccess(
	FAssetDocumentRuntimeValueDto Value,
	const FString& AuthorityId,
	const FString& StableObjectId,
	const FString& Path)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MoveTemp(Value);
	FAssetDocumentChangedRecord& Change = Outcome.ChangedSet.AddDefaulted_GetRef();
	Change.AuthorityId = AuthorityId;
	Change.StableObjectId = StableObjectId;
	Change.Path = Path;
	Change.ChangeKind = TEXT("Update");
	return Outcome;
}

bool IsParentCycle(const UBlackboardData& Blackboard, const UBlackboardData& Candidate)
{
	return &Blackboard == &Candidate || Candidate.IsChildOf(Blackboard);
}

bool HasInheritedKey(const UBlackboardData& Blackboard, const FName Name)
{
	for (const UBlackboardData* Parent = Blackboard.Parent; Parent != nullptr; Parent = Parent->Parent)
	{
		for (const FBlackboardEntry& Entry : Parent->Keys)
		{
			if (Entry.EntryName == Name) return true;
		}
	}
	return false;
}

bool IsVisibleInheritedBlackboardKey(const UBlackboardData& Blackboard, const FName Name)
{
	const FBlackboard::FKey VisibleKeyId = Blackboard.GetKeyID(Name);
	return Blackboard.GetKey(VisibleKeyId) != nullptr;
}

bool IsEngineDerivedSelfKey(const FBlackboardEntry& Entry)
{
	const UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType);
	return Entry.EntryName == FBlackboard::KeySelf
		&& ObjectKey != nullptr
		&& ObjectKey->GetClass() == UBlackboardKeyType_Object::StaticClass()
		&& ObjectKey->BaseClass == AActor::StaticClass()
		&& ObjectKey->DefaultValue == nullptr
		&& Entry.EntryDescription.IsEmpty()
		&& Entry.EntryCategory.IsNone()
		&& !Entry.bInstanceSynced;
}

#if WITH_DEV_AUTOMATION_TESTS
bool ConsumeBlackboardTestFailurePoint(const EAssetDocumentAIBlackboardTestFailurePoint FailurePoint)
{
	if (GBlackboardTestFailurePoint != FailurePoint) return false;
	GBlackboardTestFailurePoint = EAssetDocumentAIBlackboardTestFailurePoint::None;
	return true;
}

void SetBlackboardTestFailurePointInternal(
	const EAssetDocumentAIBlackboardTestFailurePoint FailurePoint)
{
	GBlackboardTestFailurePoint = FailurePoint;
}

bool ConsumeBlackboardMetadataWriteTestFailurePoint(
	const EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint FailurePoint,
	const int32 KeyTypeLeafIndex)
{
	if (GBlackboardMetadataWriteTestFailurePoint != FailurePoint
		|| (FailurePoint == EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterKeyTypeLeaf
			&& GBlackboardMetadataWriteTestKeyTypeLeafIndex != KeyTypeLeafIndex)) return false;
	GBlackboardMetadataWriteTestFailurePoint = EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::None;
	GBlackboardMetadataWriteTestKeyTypeLeafIndex = INDEX_NONE;
	return true;
}

void SetBlackboardMetadataWriteTestFailurePointInternal(
	const EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint FailurePoint,
	const int32 KeyTypeLeafIndex)
{
	GBlackboardMetadataWriteTestFailurePoint = FailurePoint;
	GBlackboardMetadataWriteTestKeyTypeLeafIndex = KeyTypeLeafIndex;
}

void BeginBlackboardMetadataGeneratorAudit()
{
	GBlackboardMetadataGeneratorAudit.Reset();
	GBlackboardMetadataGeneratorAuditActive = true;
}

void RecordBlackboardMetadataGenerator(
	const UObject& Object,
	const TSet<FString>* PropertyAllowList)
{
	if (!GBlackboardMetadataGeneratorAuditActive) return;
	TArray<FString> Names;
	if (PropertyAllowList != nullptr)
	{
		for (const FString& Name : *PropertyAllowList) Names.Add(Name);
		Names.Sort();
	}
	GBlackboardMetadataGeneratorAudit.Add(FString::Printf(TEXT("%s|%s"),
		Cast<const UBlackboardData>(&Object) != nullptr ? TEXT("Blackboard") : TEXT("KeyType"),
		*FString::Join(Names, TEXT(","))));
}

TArray<FString> EndBlackboardMetadataGeneratorAudit()
{
	GBlackboardMetadataGeneratorAuditActive = false;
	return MoveTemp(GBlackboardMetadataGeneratorAudit);
}
#endif

FAssetDocumentRuntimeValueDto MakeParentSetV2Value(
	const FAssetDocumentAIBlackboardParentRequest& Request)
{
	FAssetDocumentRuntimeValueDto ParentValue;
	if (Request.bClearParent)
	{
		ParentValue.Kind = EAssetDocumentRuntimeValueKind::Null;
	}
	else
	{
		ParentValue = StringValue(Request.ParentPath);
	}
	return ObjectValue({TEXT("parent_path_or_null")}, {MoveTemp(ParentValue)});
}

FAssetDocumentPrimitiveOutcome MakeParentSetV2NoOpSuccess(
	const FAssetDocumentAIBlackboardParentRequest& Request)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeParentSetV2Value(Request);
	return Outcome;
}

struct FParentSetV2MutationContext
{
	UBlackboardData* TargetBlackboard = nullptr;
	UBlackboardData* DesiredParent = nullptr;
	TSharedPtr<IPropertyRowGenerator> Rows;
	TSharedPtr<IPropertyHandle> ParentHandle;
	const UObject* CurrentReference = nullptr;
	FString ParentStablePath;
};

TOptional<FAssetDocumentPrimitiveOutcome> ValidateParentSetV2Cycle(
	const FParentSetV2MutationContext& Context)
{
	if (Context.DesiredParent == Context.TargetBlackboard
		|| Context.DesiredParent->IsChildOf(*Context.TargetBlackboard))
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentCycle"),
			TEXT("The requested Blackboard Parent would introduce a cycle."));
	}
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PreflightParentSetV2Target(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	FParentSetV2MutationContext& Context)
{
	if (Identity.GetCanonicalObjectPath() != Request.BlackboardPath)
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The mutable Asset lease does not identify the requested Blackboard."));
	}
	UObject& TargetObject = Identity.GetObject();
	if (TargetObject.GetClass() != UBlackboardData::StaticClass())
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentClassMismatch"),
			TEXT("The requested target is not an exact BlackboardData asset."));
	}
	Context.TargetBlackboard = static_cast<UBlackboardData*>(&TargetObject);
	if (!IsEditorPropertyContext(Context.TargetBlackboard))
	{
		return Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard Parent mutation requires the Editor game thread."));
	}
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PreflightParentSetV2DesiredParent(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	FParentSetV2MutationContext& Context)
{
	if (Request.bClearParent)
	{
		return {};
	}
	UObject* DesiredObject = FSoftObjectPath(Request.ParentPath).ResolveObject();
	if (DesiredObject == nullptr)
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentUnavailable"),
			TEXT("The requested Parent Blackboard is unavailable."));
	}
	if (DesiredObject->GetPathName() != Request.ParentPath)
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentUnavailable"),
			TEXT("The requested Parent Blackboard is unavailable."));
	}
	if (DesiredObject->GetClass() != UBlackboardData::StaticClass())
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentClassMismatch"),
			TEXT("The requested Parent is not an exact BlackboardData asset."));
	}
	Context.DesiredParent = static_cast<UBlackboardData*>(DesiredObject);
	return ValidateParentSetV2Cycle(Context);
}

TOptional<FAssetDocumentPrimitiveOutcome> PreflightParentSetV2(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	FParentSetV2MutationContext& Context)
{
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		PreflightParentSetV2Target(Identity, Request, Context))
	{
		return FailureOutcome;
	}
	return PreflightParentSetV2DesiredParent(Request, Context);
}

TOptional<FAssetDocumentPrimitiveOutcome> PrepareParentSetV2PropertyRows(
	FParentSetV2MutationContext& Context)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2BeforePropertyRows))
	{
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The test-only seam rejected the exact Parent PropertyEditor rows."));
	}
#endif
	const TSet<FString> ParentAllowList = {TEXT("Parent")};
	if (!TryMakeRows(*Context.TargetBlackboard, Context.Rows, &ParentAllowList))
	{
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact Parent PropertyEditor authority is unavailable."));
	}
	FName ParentPropertyName = GET_MEMBER_NAME_CHECKED(UBlackboardData, Parent);
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2MissingPropertyHandle))
	{
		ParentPropertyName = TEXT("__AssetDocumentTestMissingParent");
	}
#endif
	Context.ParentHandle = FindUniqueOwnedPropertyHandle(
		*Context.Rows, *Context.TargetBlackboard,
		ParentPropertyName);
	if (!Context.ParentHandle.IsValid())
	{
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact Blackboard Parent property handle is unavailable."));
	}
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> ReadParentSetV2CurrentReference(
	FParentSetV2MutationContext& Context)
{
	if (Context.ParentHandle->GetValue(Context.CurrentReference) != FPropertyAccess::Success)
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"),
			TEXT("The current Blackboard Parent readback is unavailable."));
	}
	if (Context.CurrentReference != nullptr
		&& Context.CurrentReference->GetClass() != UBlackboardData::StaticClass())
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"),
			TEXT("The current Blackboard Parent readback is unavailable."));
	}
	return {};
}

bool IsParentSetV2NoOp(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	const FParentSetV2MutationContext& Context)
{
	if (Request.bClearParent)
	{
		return Context.CurrentReference == nullptr;
	}
	return Context.CurrentReference == Context.DesiredParent
		&& Context.CurrentReference->GetPathName() == Request.ParentPath;
}

TOptional<FAssetDocumentPrimitiveOutcome> PrepareParentSetV2Authority(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	FParentSetV2MutationContext& Context)
{
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		PrepareParentSetV2PropertyRows(Context))
	{
		return FailureOutcome;
	}
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		ReadParentSetV2CurrentReference(Context))
	{
		return FailureOutcome;
	}
	if (IsParentSetV2NoOp(Request, Context))
	{
		return MakeParentSetV2NoOpSuccess(Request);
	}
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> CaptureParentSetV2OrderedPreimages(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentAtomicTransaction& Transaction,
	const FParentSetV2MutationContext& Context)
{
	const TCHAR* DerivedClosureComponent =
		TEXT("assetdocument.ai.blackboard.parent.derived_closure.v2");
	const FAssetDocumentEditorTypedPreimageResult DerivedCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			Lease, *Context.TargetBlackboard, {DerivedClosureComponent}, Transaction);
	if (!DerivedCapture.bSucceeded)
	{
		return ParentSetFailureFromPreimage(DerivedCapture);
	}
	const TCHAR* ParentReferenceComponent =
		TEXT("assetdocument.ai.blackboard.parent.reference.v2");
	const FAssetDocumentEditorTypedPreimageResult ParentCapture =
		CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
			Identity, *Context.TargetBlackboard, Context.Rows.ToSharedRef(),
			Context.ParentHandle.ToSharedRef(), {ParentReferenceComponent}, Transaction);
	if (!ParentCapture.bSucceeded)
	{
		return ParentSetFailureFromPreimage(ParentCapture);
	}
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> EnlistParentSetV2Footprint(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction,
	FParentSetV2MutationContext& Context)
{
	const TCHAR* DerivedClosureComponent =
		TEXT("assetdocument.ai.blackboard.parent.derived_closure.v2");
	const TCHAR* ParentReferenceComponent =
		TEXT("assetdocument.ai.blackboard.parent.reference.v2");
	const FString KeysStablePath = FString::Printf(
		TEXT("asset:%s/Keys[*]"), *Request.BlackboardPath);
	Context.ParentStablePath = FString::Printf(
		TEXT("asset:%s/Parent"), *Request.BlackboardPath);
	const FAssetDocumentContractValidationResult FootprintResult =
		Transaction.EnlistFootprint({
			MakeFootprint(KeysStablePath, DerivedClosureComponent),
			MakeFootprint(Context.ParentStablePath, ParentReferenceComponent)});
	if (!FootprintResult.bValid)
	{
		return ParentSetFailureWithLowerDetail(TEXT("AIProvider.OperationRejected"),
			TEXT("The active transaction rejected the complete Blackboard footprint."),
			FootprintResult.Diagnostics);
	}
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> ApplyParentSetV2ConstSetValue(
	const FParentSetV2MutationContext& Context)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2BeforeSetValue))
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentWriteRejected"),
			TEXT("The test-only seam rejected the exact Parent SetValue."));
	}
#endif
	const UObject* const DesiredReference = Context.DesiredParent;
	if (Context.ParentHandle->SetValue(
		DesiredReference, EPropertyValueSetFlags::NotTransactable)
		!= FPropertyAccess::Success)
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentWriteRejected"),
			TEXT("The exact Blackboard Parent SetValue was rejected."));
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2AfterSetValue))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test-only seam rejected the mutation after Parent SetValue."));
	}
#endif
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PropagateParentSetV2DerivedKeys(
	const FParentSetV2MutationContext& Context)
{
	Context.TargetBlackboard->PropagateKeyChangesToDerivedBlackboardAssets();
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2AfterPropagation))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test-only seam rejected the derived Blackboard refresh."));
	}
#endif
	return {};
}

bool IsExactParentSetV2Readback(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	const FParentSetV2MutationContext& Context,
	const UObject* ParentReadback)
{
	if (ParentReadback != Context.DesiredParent)
	{
		return false;
	}
	if (ParentReadback == nullptr)
	{
		return true;
	}
	return ParentReadback->GetClass() == UBlackboardData::StaticClass()
		&& ParentReadback->GetPathName() == Request.ParentPath;
}

bool IsExactParentSetV2TargetIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	const FParentSetV2MutationContext& Context)
{
	UObject& TargetObject = Identity.GetObject();
	return &TargetObject == Context.TargetBlackboard
		&& TargetObject.GetClass() == UBlackboardData::StaticClass()
		&& TargetObject.GetPathName() == Request.BlackboardPath;
}

TOptional<FAssetDocumentPrimitiveOutcome> ReadbackParentSetV2Reference(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	const FParentSetV2MutationContext& Context)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2BeforeReadback))
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"),
			TEXT("The test-only seam rejected the final Parent readback."));
	}
#endif
	if (!IsExactParentSetV2TargetIdentity(Identity, Request, Context))
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The target Blackboard identity changed before final readback."));
	}
	const UObject* ParentReadback = nullptr;
	if (Context.ParentHandle->GetValue(ParentReadback) != FPropertyAccess::Success)
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"),
			TEXT("The final Blackboard Parent readback does not match the request."));
	}
	if (!IsExactParentSetV2Readback(Request, Context, ParentReadback))
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"),
			TEXT("The final Blackboard Parent readback does not match the request."));
	}
	return {};
}

FAssetDocumentPrimitiveOutcome MakeParentSetV2ChangedSuccess(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	const FParentSetV2MutationContext& Context)
{
	FAssetDocumentPrimitiveOutcome Outcome = MakeSuccess(
		MakeParentSetV2Value(Request), TEXT("ai.blackboard.parent.set.v2"),
		Request.BlackboardPath, Context.ParentStablePath);
	bool bChangedSetMismatch = Outcome.HandleResults.Num() != 0
		|| Outcome.ChangedSet.Num() != 1;
#if WITH_DEV_AUTOMATION_TESTS
	bChangedSetMismatch = bChangedSetMismatch || ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::ParentSetV2ChangedSetMismatch);
#endif
	if (bChangedSetMismatch)
	{
		return Failure(TEXT("AIProvider.Blackboard.ParentChangedSetMismatch"),
			TEXT("The exact Blackboard Parent ChangedSet could not be proven."));
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome ApplyParentSetV2WriteStages(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	const FParentSetV2MutationContext& Context)
{
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		ApplyParentSetV2ConstSetValue(Context))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		PropagateParentSetV2DerivedKeys(Context))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		ReadbackParentSetV2Reference(Identity, Request, Context))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}
	return MakeParentSetV2ChangedSuccess(Request, Context);
}

FAssetDocumentPrimitiveOutcome CaptureAndApplyParentSetV2MutationStages(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction,
	FParentSetV2MutationContext& Context)
{
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		CaptureParentSetV2OrderedPreimages(Identity, Lease, Transaction, Context))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		EnlistParentSetV2Footprint(Request, Transaction, Context))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}
	return ApplyParentSetV2WriteStages(Identity, Request, Context);
}

FAssetDocumentPrimitiveOutcome ApplyParentSetV2WithIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FParentSetV2MutationContext Context;
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		PreflightParentSetV2(Identity, Request, Context))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}
	if (TOptional<FAssetDocumentPrimitiveOutcome> TerminalOutcome =
		PrepareParentSetV2Authority(Request, Context))
	{
		return MoveTemp(TerminalOutcome.GetValue());
	}
	return CaptureAndApplyParentSetV2MutationStages(
		Identity, Lease, Request, Transaction, Context);
}

struct FParentSetV2InvocationContext
{
	FAssetDocumentAIBlackboardParentRequest Request;
	FAssetDocumentVerifiedLeaseResult LeaseResult;
};

TOptional<FAssetDocumentPrimitiveOutcome> PrepareParentSetV2Invocation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction,
	FParentSetV2InvocationContext& Context)
{
	if (!ParseAssetDocumentAIBlackboardParentRequest(Invocation, Context.Request))
	{
		return Failure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The Blackboard Parent v2 request is not canonical."));
	}
	if (ActiveTransaction == nullptr)
	{
		return Failure(TEXT("AIProvider.InvalidTransaction"),
			TEXT("Blackboard Parent v2 requires an active Atomic transaction."));
	}
	if (!IsEditorPropertyContext())
	{
		return Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard Parent v2 requires the Editor game thread."));
	}
	Context.LeaseResult = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (Context.LeaseResult.Lease == nullptr)
	{
		return ParentSetFailureWithLowerDetail(TEXT("AIProvider.InvalidHandle"),
			TEXT("A matching AI mutable Asset lease is required."),
			Context.LeaseResult.Diagnostics);
	}
	return {};
}

bool RuntimeValueEquals(
	const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right)
{
	if (Left.Kind != Right.Kind || Left.BooleanValue != Right.BooleanValue
		|| Left.IntegerValue != Right.IntegerValue || Left.NumberValue != Right.NumberValue
		|| Left.StringValue != Right.StringValue || Left.BytesValue != Right.BytesValue
		|| Left.ObjectKeys != Right.ObjectKeys || Left.Elements.Num() != Right.Elements.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Elements.Num(); ++Index)
	{
		if (!RuntimeValueEquals(Left.Elements[Index], Right.Elements[Index])) return false;
	}
	return true;
}


} // namespace AssetDocumentAIBlackboardMutation

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardEntryUpdateExisting(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	FAssetDocumentAIBlackboardEntryUpdateExistingRequest Parsed;
	const EAssetDocumentAIBlackboardEntryUpdateParseResult ParseResult =
		ParseAssetDocumentAIBlackboardEntryUpdateExistingRequest(Invocation, Parsed);
	if (ParseResult == EAssetDocumentAIBlackboardEntryUpdateParseResult::InvalidEnvelope)
	{
		return AssetDocumentAIBlackboardMutation::Failure(
			TEXT("AIProvider.InvalidValueShape"),
			TEXT("The Blackboard entry update envelope is not canonical."));
	}
	if (ParseResult == EAssetDocumentAIBlackboardEntryUpdateParseResult::InvalidEntry)
	{
		return AssetDocumentAIBlackboardMutation::Failure(
			TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("The embedded Blackboard entry does not satisfy BlackboardEntryV2."));
	}

	AssetDocumentAIBlackboardMutation::FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest Request;
	Request.BlackboardPath = MoveTemp(Parsed.BlackboardPath);
	Request.KeyName = MoveTemp(Parsed.KeyName);
	Request.ExpectedClassPath = MoveTemp(Parsed.ExpectedClassPath);
	Request.CanonicalProperties = MoveTemp(Parsed.CanonicalProperties);
	Request.Description = MoveTemp(Parsed.Description);
	Request.Category = MoveTemp(Parsed.Category);
	Request.bInstanceSynced = Parsed.bInstanceSynced;
	return AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
		Invocation, Request, Handles, ActiveTransaction);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardEntryAdd(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentAIBlackboardMutation;
	FAssetDocumentAIBlackboardEntryAddRequest Request;
	const EAssetDocumentAIBlackboardEntryAddParseResult ParseResult =
		ParseAssetDocumentAIBlackboardEntryAddRequest(Invocation, Request);
	if (ParseResult != EAssetDocumentAIBlackboardEntryAddParseResult::Succeeded)
	{
		const bool bInvalidEnvelope =
			ParseResult == EAssetDocumentAIBlackboardEntryAddParseResult::InvalidEnvelope;
		return Failure(bInvalidEnvelope ? TEXT("AIProvider.InvalidValueShape")
			: TEXT("AIProvider.Blackboard.EntrySchemaInvalid"), bInvalidEnvelope
				? TEXT("The entry-add envelope is not canonical.")
				: TEXT("The entry-add payload is invalid."));
	}
	if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
		return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("Entry add requires the exact active transaction."));
	if (!IsEditorPropertyContext())
		return Failure(TEXT("AIProvider.InvalidContext"), TEXT("Entry add requires the Editor game thread."));
	FAssetDocumentVerifiedLeaseResult LeaseResult =
		AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (LeaseResult.Lease == nullptr)
		return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("Entry add requires one matching mutable Asset lease."), LeaseResult.Diagnostics);
	return ApplyWithCurrentAssetIdentity(
		*LeaseResult.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			return ApplyAssetDocumentAIBlackboardEntryAddWithIdentity(
				Identity, *LeaseResult.Lease, Request, *ActiveTransaction);
		});
}

FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardParentSetV2(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentAIBlackboardMutation;
	FParentSetV2InvocationContext Context;
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		PrepareParentSetV2Invocation(
			Invocation, Handles, ActiveTransaction, Context))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}

	return ApplyWithCurrentAssetIdentity(
		*Context.LeaseResult.Lease,
		[&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			return ApplyParentSetV2WithIdentity(
				Identity, *Context.LeaseResult.Lease,
				Context.Request, *ActiveTransaction);
		});
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardParentSetV2(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	return ApplyAssetDocumentAIBlackboardParentSetV2(
		Invocation, Handles, ActiveTransaction);
}

#if WITH_DEV_AUTOMATION_TESTS
void SetAssetDocumentAIBlackboardTestFailurePoint(
	const EAssetDocumentAIBlackboardTestFailurePoint FailurePoint)
{
	AssetDocumentAIBlackboardMutation::SetBlackboardTestFailurePointInternal(
		FailurePoint);
}
#endif
