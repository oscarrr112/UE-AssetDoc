// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"

#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BTService.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "EditorCategoryUtils.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "IPropertyTypeCustomization.h"
#include "ISinglePropertyView.h"
#include "IDetailChildrenBuilder.h"
#include "Modules/ModuleManager.h"
#include "ObjectEditorUtils.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include <type_traits>
#include "UObject/UnrealType.h"

namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback
{
namespace
{
// Stock ValueOr editor widgets repair missing or incompatible keys while building
// their header. A readback generator must expose handles without running that UI.
class FReadOnlyValueOrCustomization final : public IPropertyTypeCustomization
{
public:
	void CustomizeHeader(TSharedRef<IPropertyHandle>, FDetailWidgetRow&, IPropertyTypeCustomizationUtils&) override {}
	void CustomizeChildren(TSharedRef<IPropertyHandle> Handle, IDetailChildrenBuilder& Children,
		IPropertyTypeCustomizationUtils&) override
	{
		uint32 Count = 0;
		if (Handle->GetNumChildren(Count) != FPropertyAccess::Success) return;
		for (uint32 Index = 0; Index < Count; ++Index)
			if (const auto Child = Handle->GetChildHandle(Index)) Children.AddProperty(Child.ToSharedRef());
	}
};

// Stock selector headers can select a key and dirty their owner while building
// rows. Preserve the same handles without invoking that editing customization.
class FReadOnlySelectorCustomization final : public IPropertyTypeCustomization
{
public:
	void CustomizeHeader(TSharedRef<IPropertyHandle> Handle, FDetailWidgetRow& Row,
		IPropertyTypeCustomizationUtils&) override
	{
		Row.NameContent()[Handle->CreatePropertyNameWidget()]
			.ValueContent()[Handle->CreatePropertyValueWidget()];
	}
	void CustomizeChildren(TSharedRef<IPropertyHandle>, IDetailChildrenBuilder&,
		IPropertyTypeCustomizationUtils&) override {}
};
}
void ConfigureReadOnlyValueOrRows(IPropertyRowGenerator& Rows)
{
	for (const TCHAR* Family : {TEXT("Bool"), TEXT("Class"), TEXT("Enum"), TEXT("Float"), TEXT("Int32"),
		TEXT("Name"), TEXT("String"), TEXT("Object"), TEXT("Rotator"), TEXT("Struct"), TEXT("Vector")})
		Rows.RegisterInstancedCustomPropertyTypeLayout(FName(FString(TEXT("ValueOrBBKey_")) + Family),
			FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() -> TSharedRef<IPropertyTypeCustomization>
			{ return MakeShared<FReadOnlyValueOrCustomization>(); }));
	Rows.RegisterInstancedCustomPropertyTypeLayout(FBlackboardKeySelector::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() -> TSharedRef<IPropertyTypeCustomization>
		{ return MakeShared<FReadOnlySelectorCustomization>(); }));
}

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto IntegerValue(const int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto NumberValue(const double Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number;
	Result.NumberValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto BooleanValue(const bool Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto NullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto ArrayValue(TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Values);
	return Result;
}

bool ReadScalarProperty(const IPropertyHandle& Handle,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	const FProperty* Property = Handle.GetProperty();
	if (CastField<FBoolProperty>(Property) != nullptr)
	{
		bool Value = false;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = BooleanValue(Value);
		return true;
	}
	if (CastField<FIntProperty>(Property) != nullptr)
	{
		int32 Value = 0;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = IntegerValue(Value);
		return true;
	}
	if (CastField<FInt64Property>(Property) != nullptr)
	{
		int64 Value = 0;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = IntegerValue(Value);
		return true;
	}
	if (CastField<FFloatProperty>(Property) != nullptr)
	{
		float Value = 0.0f;
		if (Handle.GetValue(Value) != FPropertyAccess::Success || !FMath::IsFinite(Value)) return false;
		OutValue = NumberValue(Value);
		return true;
	}
	if (CastField<FDoubleProperty>(Property) != nullptr)
	{
		double Value = 0.0;
		if (Handle.GetValue(Value) != FPropertyAccess::Success || !FMath::IsFinite(Value)) return false;
		OutValue = NumberValue(Value);
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

bool IsAuthoredProperty(const IPropertyHandle& Handle)
{
	const FProperty* Property = Handle.GetProperty();
	return Property != nullptr && Handle.IsValidHandle()
		&& Property->HasAnyPropertyFlags(CPF_Edit)
		&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst);
}

bool IsHiddenCategoryAuthoredScalar(const UObject& Object, const FProperty& Property)
{
	const auto* DeclaringClass = Cast<UClass>(Property.GetOwnerStruct());
	if (!DeclaringClass || !Object.IsA(DeclaringClass) || Property.GetOwnerProperty() != &Property
		|| FindFProperty<FProperty>(Object.GetClass(), Property.GetFName()) != &Property
		|| Property.ArrayDim != 1 || !Property.HasAnyPropertyFlags(CPF_Edit)
		|| Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst)) return false;
	// This existing public service bool has a stock UI edit condition. Admit its
	// actual declaration; the typed handle still checks native editability below.
	if (Property.HasMetaData(TEXT("EditCondition"))
		&& &Property != FindFProperty<FBoolProperty>(UBTService::StaticClass(), TEXT("bCallTickOnSearchStart"))) return false;
	// A standalone view runs global type headers. Restrict it to the existing
	// primitive scalar codec, never a selector, ValueOr, container or inline object.
	if (!CastField<FBoolProperty>(&Property) && !CastField<FNumericProperty>(&Property)
		&& !CastField<FNameProperty>(&Property) && !CastField<FStrProperty>(&Property)
		&& !CastField<FTextProperty>(&Property)) return false;
	return FEditorCategoryUtils::IsCategoryHiddenFromClass(Object.GetClass(),
		FObjectEditorUtils::GetCategoryFName(&Property).ToString());
}

bool MakeHiddenCategoryScalarHandle(UObject& Object, const FProperty& Property,
	FHiddenCategoryScalarHandle& Out)
{
	Out = {};
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet() || IsGarbageCollecting()
		|| !IsHiddenCategoryAuthoredScalar(Object, Property)) return false;
	auto* Module = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (!Module) return false;
	FSinglePropertyParams Params;
	Params.NamePlacement = EPropertyNamePlacement::Hidden;
	Params.bHideAssetThumbnail = true;
	Params.bHideResetToDefault = true;
	const auto View = Module->CreateSingleProperty(&Object, Property.GetFName(), Params);
	const auto Handle = View.IsValid() ? View->GetPropertyHandle() : nullptr;
	TArray<UObject*> Owners;
	if (Handle.IsValid()) Handle->GetOuterObjects(Owners);
	if (!Handle.IsValid() || !IsAuthoredProperty(*Handle) || Handle->GetProperty() != &Property
		|| Handle->GeneratePathToProperty() != Property.GetName()
		|| Owners.Num() != 1 || Owners[0] != &Object || Handle->IsEditConst()) return false;
	Out.View = View;
	Out.Handle = Handle;
	return true;
}

namespace
{
template<typename T>
bool ReadInteger(const IPropertyHandle& Handle, FPropertyReadContext& Context, FAssetDocumentRuntimeValueDto& OutValue)
{
	T Value = 0;
	if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
	if constexpr (std::is_same_v<T, uint64>)
	{
		if (Value > static_cast<uint64>(MAX_int64)) { Context.bOverflow = true; return false; }
	}
	OutValue = IntegerValue(static_cast<int64>(Value));
	return true;
}

bool BuildProperties(UObject& Object, FPropertyReadContext& Context, int32 Depth,
 FAssetDocumentRuntimeValueDto& OutProperties);

bool ReadReference(const IPropertyHandle& Handle, FPropertyReadContext& Context, int32 Depth,
 FAssetDocumentRuntimeValueDto& OutValue)
{
	const FProperty* Property = Handle.GetProperty();
	bool bClass = CastField<FClassProperty>(Property) != nullptr
		|| CastField<FSoftClassProperty>(Property) != nullptr;
	FString Path;
	if (CastField<FSoftObjectProperty>(Property))
	{
		// Read only this exact reference leaf. Never resolve or load a soft target.
		FString ExportPath;
		if (Handle.GetValueAsFormattedString(ExportPath) != FPropertyAccess::Success) return false;
		Path = FPackageName::ExportTextPathToObjectPath(ExportPath);
		if (Path == TEXT("None")) Path.Reset();
	}
	else
	{
		const UObject* Value = nullptr;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		if (Value && Context.Version == EAuthoredGraphReadbackVersion::V2
			&& Property->HasAnyPropertyFlags(CPF_InstancedReference) && Context.RootObject
			&& Value->IsIn(Context.RootObject) && !Cast<UClass>(Value))
		{
			FAssetDocumentRuntimeValueDto Properties;
			if (!BuildProperties(*const_cast<UObject*>(Value), Context, Depth + 1, Properties)) return false;
			OutValue = ObjectValue({TEXT("Class"), TEXT("Properties")},
				{StringValue(Value->GetClass()->GetPathName()), MoveTemp(Properties)});
			return true;
		}
		if (Value) { Path = Value->GetPathName(); bClass |= Cast<UClass>(Value) != nullptr; }
	}
	if (Path.IsEmpty()) { OutValue = NullValue(); return true; }
	const FSoftObjectPath Canonical(Path);
	if (!Canonical.IsValid() || Canonical.ToString() != Path) return false;
	OutValue = ObjectValue({TEXT("Kind"), TEXT("Path")},
		{StringValue(bClass ? TEXT("ClassRef") : TEXT("AssetRef")), StringValue(Path)});
	return true;
}

bool ReadExtendedLeaf(const IPropertyHandle& Handle, FPropertyReadContext& Context, int32 Depth, FAssetDocumentRuntimeValueDto& OutValue)
{
	const FProperty* Property = Handle.GetProperty();
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		// GetValueAsFormattedString requests non-display text. PropertyEditor
		// canonicalizes enum leaves to their declared names even when numeric
		// port flags were supplied, so resolve that exact token through UEnum.
		// The uint8 typed getter cannot preserve a wide enum's underlying value.
		FString Token;
		const UEnum* Enum = EnumProperty->GetEnum();
		if (!Enum || Handle.GetValueAsFormattedString(Token, PPF_Copy) != FPropertyAccess::Success) return false;
		const int32 Index = Enum->GetIndexByNameString(Token);
		if (Index == INDEX_NONE || Enum->GetNameStringByIndex(Index) != Token) return false;
		OutValue = IntegerValue(Enum->GetValueByIndex(Index));
		return true;
	}
	if (CastField<FInt8Property>(Property)) return ReadInteger<int8>(Handle, Context, OutValue);
	if (CastField<FInt16Property>(Property)) return ReadInteger<int16>(Handle, Context, OutValue);
	if (CastField<FUInt16Property>(Property)) return ReadInteger<uint16>(Handle, Context, OutValue);
	if (CastField<FUInt32Property>(Property)) return ReadInteger<uint32>(Handle, Context, OutValue);
	if (CastField<FUInt64Property>(Property)) return ReadInteger<uint64>(Handle, Context, OutValue);
	if (CastField<FByteProperty>(Property)) return ReadInteger<uint8>(Handle, Context, OutValue);
	if (CastField<FTextProperty>(Property))
	{
		FText Value;
		if (Handle.GetValue(Value) != FPropertyAccess::Success) return false;
		OutValue = StringValue(Value.ToString());
		return true;
	}
	if (CastField<FObjectProperty>(Property) || CastField<FSoftObjectProperty>(Property))
		return ReadReference(Handle, Context, Depth, OutValue);
	return ReadScalarProperty(Handle, OutValue);
}

bool GatherRoots(const TArray<TSharedRef<IDetailTreeNode>>& Nodes, UObject& Owner,
	TMap<FString, TSharedRef<IPropertyHandle>>& OutHandles, int32& Remaining, bool& bOverflow)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		if (--Remaining < 0) { bOverflow = true; return false; }
		TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid())
		{
			// Recover the direct UObject field even across a synthetic structure
			// wrapper (which may have no FProperty at all).
			TSharedPtr<IPropertyHandle> RootField;
			for (int32 Depth = 0; Depth <= 64; ++Depth)
			{
				const FProperty* Property = Handle->GetProperty();
				if (Property && Property->GetOwnerProperty() == Property
					&& Cast<UClass>(Property->GetOwnerStruct())) RootField = Handle;
				const TSharedPtr<IPropertyHandle> Parent = Handle->GetParentHandle();
				if (!Parent.IsValid()) break;
				Handle = Parent;
				if (Depth == 64) { bOverflow = true; return false; }
			}
			TArray<UObject*> Outers;
			if (RootField.IsValid()) RootField->GetOuterObjects(Outers);
			if (RootField.IsValid() && IsAuthoredProperty(*RootField)
				&& Outers.Num() == 1 && Outers[0] == &Owner)
				OutHandles.Add(RootField->GetProperty()->GetName(), RootField.ToSharedRef());
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		if (!GatherRoots(Children, Owner, OutHandles, Remaining, bOverflow)) return false;
	}
	return true;
}
}

bool ReadAuthoredProperty(IPropertyHandle& Handle, FPropertyReadContext& Context,
	const int32 Depth, FAssetDocumentRuntimeValueDto& OutValue)
{
	if (Depth > 64 || --Context.RemainingNodes < 0)
	{ Context.bOverflow = true; return false; }
	if (!Handle.IsValidHandle() || !Handle.GetProperty()) return false;
	if (Handle.AsArray().IsValid() || Handle.AsMap().IsValid() || Handle.AsSet().IsValid())
		return ReadAuthoredContainer(Handle, Context, Depth, OutValue);
	if (CastField<FStructProperty>(Handle.GetProperty()))
		return ReadAuthoredStruct(Handle, Context, Depth, OutValue);
	return ReadExtendedLeaf(Handle, Context, Depth, OutValue);
}

namespace
{
bool BuildProperties(UObject& Object, FPropertyReadContext& Context, int32 Depth,
 FAssetDocumentRuntimeValueDto& OutProperties)
{
	OutProperties = {};
	if (Depth > 64 || Context.ActiveObjects.Contains(&Object)) return false;
	Context.ActiveObjects.Add(&Object);
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet() || IsGarbageCollecting()) return false;
	FPropertyEditorModule* const PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr) return false;
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = true;
	const TSharedPtr<IPropertyRowGenerator> Rows = PropertyEditor->CreatePropertyRowGenerator(Args);
	if (!Rows.IsValid()) return false;
	ConfigureReadOnlyValueOrRows(*Rows);
	Rows->SetObjects({&Object});
	TMap<FString, TSharedRef<IPropertyHandle>> Handles;
	int32 RemainingRows = 4096;
	bool bRowOverflow = false;
	if (!GatherRoots(Rows->GetRootTreeNodes(), Object, Handles, RemainingRows, bRowOverflow))
	{ Context.bOverflow |= bRowOverflow; return false; }
	TArray<FHiddenCategoryScalarHandle> HiddenScalars;
	if (Context.Version == EAuthoredGraphReadbackVersion::V2)
		for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
		{
			if (Handles.Contains(It->GetName()) || !IsHiddenCategoryAuthoredScalar(Object, **It)) continue;
			if (--RemainingRows < 0) { Context.bOverflow = true; return false; }
			FHiddenCategoryScalarHandle Scalar;
			if (!MakeHiddenCategoryScalarHandle(Object, **It, Scalar)) return false;
			Handles.Add(It->GetName(), Scalar.Handle.ToSharedRef());
			HiddenScalars.Add(MoveTemp(Scalar));
		}
	TArray<FString> Keys;
	Handles.GetKeys(Keys);
	Keys.Sort();
	TArray<FAssetDocumentRuntimeValueDto> Values;
	for (const FString& Key : Keys)
	{
		FAssetDocumentRuntimeValueDto Value;
		if (!ReadAuthoredProperty(*Handles.FindChecked(Key), Context, Depth + 1, Value))
		{
			UE_LOG(LogTemp, Display, TEXT("BehaviorTree authored property rejected: %s (%s), overflow=%d"),
				*Key, *Handles.FindChecked(Key)->GeneratePathToProperty(), Context.bOverflow);
			return false;
		}
		Values.Add(MoveTemp(Value));
	}
	FAssetDocumentRuntimeValueDto Result = ObjectValue(MoveTemp(Keys), MoveTemp(Values));
	if (!ValidateAssetDocumentRuntimeValue(Result).bValid)
	{ Context.bOverflow = true; return false; }
	OutProperties = MoveTemp(Result);
	Context.ActiveObjects.Remove(&Object);
	return true;
}
}

bool BuildAuthoredProperties(UObject& Object, FAssetDocumentRuntimeValueDto& OutProperties,
 bool* OutOverflow, EAuthoredGraphReadbackVersion Version)
{
 FPropertyReadContext Context;
 Context.Version = Version;
 Context.RootObject = &Object;
 const bool bResult = BuildProperties(Object, Context, 0, OutProperties);
 if (OutOverflow) *OutOverflow = Context.bOverflow;
 return bResult;
}
}
