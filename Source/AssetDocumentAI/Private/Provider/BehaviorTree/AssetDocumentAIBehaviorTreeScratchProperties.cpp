// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeScratchAuthoring.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "PropertyCustomizationHelpers.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocumentAI::BehaviorTreeScratchAuthoring
{
namespace
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
namespace Readback = BehaviorTreeAuthoredReadback;
constexpr auto Flags = EPropertyValueSetFlags::NotTransactable;

bool FindRows(const TArray<TSharedRef<IDetailTreeNode>>& Nodes, UObject& Object,
 TMap<FName, TSharedPtr<IPropertyHandle>>& Handles, int32& Budget)
{
 for (const auto& Node : Nodes)
 {
  if (--Budget < 0) return false;
  auto Handle = Node->CreatePropertyHandle();
  for (int32 Depth = 0; Handle.IsValid() && Depth < 64; ++Depth)
  {
   const FProperty* Property = Handle->GetProperty();
   TArray<UObject*> Outers;
   Handle->GetOuterObjects(Outers);
   if (Property && Property->GetOwnerProperty() == Property && Cast<UClass>(Property->GetOwnerStruct())
    && Outers.Num() == 1 && Outers[0] == &Object) Handles.Add(Property->GetFName(), Handle);
   Handle = Handle->GetParentHandle();
  }
  TArray<TSharedRef<IDetailTreeNode>> Children;
  Node->GetChildren(Children, true);
  if (!FindRows(Children, Object, Handles, Budget)) return false;
 }
 return true;
}

TSharedPtr<IPropertyRowGenerator> RowsFor(UObject& Object, bool bHidden,
 const TSet<FString>& Fields, TMap<FName, TSharedPtr<IPropertyHandle>>& Handles)
{
 auto* Module = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
 if (!Module) return nullptr;
 FPropertyRowGeneratorArgs Args;
 Args.bShouldShowHiddenProperties = bHidden;
 Args.bAllowEditingClassDefaultObjects = false;
 TSharedPtr<IPropertyRowGenerator> Rows = Module->CreatePropertyRowGenerator(Args);
 if (!Rows.IsValid()) return nullptr;
 Readback::ConfigureReadOnlyValueOrRows(*Rows);
 Rows->SetPropertyGenerationAllowListPaths(Fields);
 Rows->SetObjects({&Object});
 int32 Budget = 4096;
 return FindRows(Rows->GetRootTreeNodes(), Object, Handles, Budget) ? Rows : nullptr;
}

template<typename T> bool SetInteger(IPropertyHandle& Handle, const FValue& Value)
{
 if (Value.Kind != EKind::Integer || static_cast<long double>(Value.IntegerValue) < static_cast<long double>(TNumericLimits<T>::Lowest())
  || static_cast<long double>(Value.IntegerValue) > static_cast<long double>(TNumericLimits<T>::Max())) return false;
 return Handle.SetValue(static_cast<T>(Value.IntegerValue), Flags) == FPropertyAccess::Success;
}

bool WriteValue(IPropertyHandle& Handle, const FValue& Value, int32 Depth, bool bContainerElement = false);

bool ExactValueOrType(const UStruct* Type)
{
#define BT_VALUE_OR_TYPE(N) if (Type == FValueOrBBKey_##N::StaticStruct()) return true;
 BT_VALUE_OR_TYPE(Bool) BT_VALUE_OR_TYPE(Class) BT_VALUE_OR_TYPE(Enum)
 BT_VALUE_OR_TYPE(Float) BT_VALUE_OR_TYPE(Int32) BT_VALUE_OR_TYPE(Name)
 BT_VALUE_OR_TYPE(String) BT_VALUE_OR_TYPE(Object) BT_VALUE_OR_TYPE(Rotator)
 BT_VALUE_OR_TYPE(Struct) BT_VALUE_OR_TYPE(Vector)
#undef BT_VALUE_OR_TYPE
 return false;
}

bool ContainsInlineObject(const FValue& Value)
{
 // Struct ValueOr literals may contain arbitrary declared structs. Do not
 // permit their inline-object codec to follow an object referenced by storage.
 if (Value.Kind == EKind::Object
  && FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Class"))
  && FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Properties"))) return true;
 for (const FValue& Child : Value.Elements) if (ContainsInlineObject(Child)) return true;
 return false;
}

bool EqualValue(const FValue& Left, const FValue& Right)
{
 if ((Left.Kind == EKind::Integer || Left.Kind == EKind::Number) && (Right.Kind == EKind::Integer || Right.Kind == EKind::Number))
  return (Left.Kind == EKind::Integer ? static_cast<long double>(Left.IntegerValue) : Left.NumberValue)
   == (Right.Kind == EKind::Integer ? static_cast<long double>(Right.IntegerValue) : Right.NumberValue);
 if (Left.Kind != Right.Kind || Left.ObjectKeys != Right.ObjectKeys || Left.Elements.Num() != Right.Elements.Num()) return false;
 if (Left.Kind == EKind::String) return Left.StringValue == Right.StringValue;
 if (Left.Kind == EKind::Boolean) return Left.BooleanValue == Right.BooleanValue;
 for (int32 Index = 0; Index < Left.Elements.Num(); ++Index) if (!EqualValue(Left.Elements[Index], Right.Elements[Index])) return false;
 return true;
}

bool IsUnchangedDisabledAuthoredValue(UObject& Object, IPropertyHandle& Handle, const FValue& Desired)
{
 const FProperty* Property = Handle.GetProperty();
 TArray<UObject*> Owners;
 Handle.GetOuterObjects(Owners);
 if (!Readback::IsAuthoredProperty(Handle) || !Handle.IsEditConst()
  || Property->GetOwnerProperty() != Property || !Cast<UClass>(Property->GetOwnerStruct())
  || FindFProperty<FProperty>(Object.GetClass(), Property->GetFName()) != Property
  || Owners.Num() != 1 || Owners[0] != &Object) return false;
 Readback::FPropertyReadContext Context;
 Context.Version = Readback::EAuthoredGraphReadbackVersion::V2;
 Context.RootObject = &Object;
 FValue Current;
 TArray<uint8> CurrentBytes, DesiredBytes;
 return Readback::ReadAuthoredProperty(Handle, Context, 0, Current) && !Context.bOverflow
  && Readback::EncodeAuthoredCanonicalValue(Current, CurrentBytes)
  && Readback::EncodeAuthoredCanonicalValue(Desired, DesiredBytes) && CurrentBytes == DesiredBytes;
}

bool WriteWholeSetStruct(IPropertyHandle& Element, const FValue& Value, int32 Depth)
{
 const auto* Property = CastField<FStructProperty>(Element.GetProperty());
 if (!Property || !Property->Struct || !Property->GetOwner<FSetProperty>() || Value.Kind != EKind::Object) return false;
 auto* Module = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
 if (!Module) return false;
 // Build the value outside the live hash table. Child setters on a live set
 // struct do not rehash its enclosing set in stock UE; the whole element setter
 // below does, because its FProperty is directly owned by FSetProperty.
 const auto Storage = MakeShared<FStructOnScope>(Property->Struct);
 const TSharedPtr<IPropertyRowGenerator> Rows = Module->CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
 if (!Rows.IsValid()) return false;
 Readback::ConfigureReadOnlyValueOrRows(*Rows);
 Rows->SetStructure(Storage);
 TMap<FString, TSharedPtr<IPropertyHandle>> Fields;
 int32 Budget = 4096;
 TFunction<bool(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather;
 Gather = [&](const auto& Nodes)
 {
  for (const auto& Node : Nodes)
  {
   if (--Budget < 0) return false;
   const auto Handle = Node->CreatePropertyHandle();
   const auto* Member = Handle.IsValid() ? Handle->GetProperty() : nullptr;
   if (Member && Member->GetOwnerProperty() == Member && Property->Struct->IsChildOf(Member->GetOwnerStruct()))
   {
    if (Readback::IsAuthoredProperty(*Handle))
    { if (Fields.Contains(Member->GetName())) return false; Fields.Add(Member->GetName(), Handle); }
   }
   else
   {
    TArray<TSharedRef<IDetailTreeNode>> Children;
    Node->GetChildren(Children, true);
    if (!Gather(Children)) return false;
   }
  }
  return true;
 };
 if (!Gather(Rows->GetRootTreeNodes())) return false;
 for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
 {
  const FString& Name = Value.ObjectKeys[Index];
  const bool bSelectorKey = Property->Struct == FBlackboardKeySelector::StaticStruct() && Name == TEXT("Key");
  const auto* Handle = Fields.Find(bSelectorKey ? TEXT("SelectedKeyName") : Name);
  if (!Handle || !Handle->IsValid() || !WriteValue(**Handle, Value.Elements[Index], Depth + 1)) return false;
 }
 FString Formatted;
 Property->Struct->ExportText(Formatted, Storage->GetStructMemory(), nullptr, nullptr, PPF_None, nullptr);
 return Element.SetValueFromFormattedString(Formatted, Flags) == FPropertyAccess::Success;
}

bool WriteContainer(IPropertyHandle& Handle, const FValue& Value, int32 Depth)
{
 auto Array = Handle.AsArray();
 auto Set = Handle.AsSet();
 auto Map = Handle.AsMap();
 TArray<FString> MapKeys;
 TArray<FValue> MapItems;
 if (Map.IsValid())
 {
  if (Value.Kind == EKind::Object)
  {
   MapKeys = Value.ObjectKeys;
   MapItems = Value.Elements;
  }
  else if (Value.Kind == EKind::Array)
  {
   const auto* Property = CastField<FMapProperty>(Handle.GetProperty());
   if (!Property || !CastField<FStrProperty>(Property->KeyProp)) return false;
   for (const FValue& Entry : Value.Elements)
   {
    if (!HasAssetDocumentAIBehaviorTreeExactFields(Entry, {TEXT("key"), TEXT("value")})) return false;
    const auto* Key = FindAssetDocumentAIBehaviorTreeField(Entry, TEXT("key"));
    if (Key->Kind != EKind::String || (!MapKeys.IsEmpty() && MapKeys.Last().Compare(Key->StringValue, ESearchCase::CaseSensitive) >= 0)) return false;
    MapKeys.Add(Key->StringValue);
    MapItems.Add(*FindAssetDocumentAIBehaviorTreeField(Entry, TEXT("value")));
   }
   // This extension only represents maps outside the original object-key domain.
   if (MapKeys.IsEmpty() || !MapKeys[0].IsEmpty()) return false;
  }
  else return false;
 }
 else if (Value.Kind != EKind::Array) return false;
 const TArray<FValue>& Items = Map.IsValid() ? MapItems : Value.Elements;
 if (Array.IsValid() && Handle.GetProperty()->ArrayDim > 1)
 {
  // Fixed reflected arrays expose IPropertyHandleArray too, but stock
  // EmptyArray/AddItem are only valid for dynamic FArrayProperty storage.
  uint32 Count = 0;
  if (Value.Elements.Num() != Handle.GetProperty()->ArrayDim
   || Array->GetNumElements(Count) != FPropertyAccess::Success || Count != static_cast<uint32>(Value.Elements.Num())) return false;
  for (uint32 Index = 0; Index < Count; ++Index)
   if (!WriteValue(*Array->GetElement(Index), Value.Elements[Index], Depth + 1, true)) return false;
  return true;
 }
 const auto Empty = Array.IsValid() ? Array->EmptyArray() : (Set.IsValid() ? Set->Empty() : Map->Empty());
 if (Empty != FPropertyAccess::Success) return false;
 TArray<int32> Order;
 for (int32 Index = 0; Index < Items.Num(); ++Index) Order.Add(Index);
 for (int32 Position = 0; Position < Order.Num(); ++Position)
 {
  const auto Added = Array.IsValid() ? Array->AddItem() : (Set.IsValid() ? Set->AddItem() : Map->AddItem());
  if (Added.GetAccessResult() != FPropertyAccess::Success || Added.GetIndex() < 0) return false;
  auto Element = Array.IsValid() ? Array->GetElement(Added.GetIndex())
   : (Set.IsValid() ? Set->GetElement(Added.GetIndex()) : Map->GetElement(Added.GetIndex()));
  if (Position == 0 && !Array.IsValid())
  {
   // AddItem needs an unused default hash/key. Put an authored default element
   // last so valid sets/maps containing it do not block the following AddItem.
   const auto DefaultHandle = Map.IsValid() ? Element->GetKeyHandle() : TSharedPtr<IPropertyHandle>(Element);
   FValue Default;
   Readback::FPropertyReadContext Context;
   if (!DefaultHandle.IsValid() || !Readback::ReadAuthoredProperty(*DefaultHandle, Context, 0, Default)) return false;
   auto IsDefault = [&](int32 Index)
   {
    if (!Map.IsValid()) return EqualValue(Default, Value.Elements[Index]);
    if (Default.Kind == EKind::Integer) return MapKeys[Index] == LexToString(Default.IntegerValue);
    return Default.Kind == EKind::String && MapKeys[Index] == Default.StringValue;
   };
   Order.StableSort([&](int32 Left, int32 Right) { return !IsDefault(Left) && IsDefault(Right); });
  }
  const int32 Index = Order[Position];
  if (Map.IsValid())
  {
   auto Key = Element->GetKeyHandle();
   if (!Key.IsValid()) return false;
   FValue KeyValue = Readback::StringValue(MapKeys[Index]);
   if (CastField<FByteProperty>(Key->GetProperty()) || CastField<FEnumProperty>(Key->GetProperty()))
   {
    int64 Number = 0;
    if (LexTryParseString(Number, *MapKeys[Index]) && LexToString(Number) == MapKeys[Index]) KeyValue = Readback::IntegerValue(Number);
    else if (const auto* Byte = CastField<FByteProperty>(Key->GetProperty()); Byte && !Byte->Enum) return false;
   }
   if (!WriteValue(*Key, KeyValue, Depth + 1, true)) return false;
  }
  if (Set.IsValid() && CastField<FStructProperty>(Element->GetProperty()))
  { if (!WriteWholeSetStruct(*Element, Items[Index], Depth + 1)) return false; }
  else if (!WriteValue(*Element, Items[Index], Depth + 1, true)) return false;
 }
 return true;
}

bool WriteReference(IPropertyHandle& Handle, const FValue& Value)
{
 if (Value.Kind == EKind::Null) return Handle.SetValue(static_cast<UObject*>(nullptr), Flags) == FPropertyAccess::Success;
 if (const auto* ClassValue = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Class")))
 {
  const auto* Property = CastField<FObjectProperty>(Handle.GetProperty());
  const auto* Properties = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Properties"));
  TArray<UObject*> Owners;
  Handle.GetOuterObjects(Owners);
  UClass* Class = ClassValue->Kind == EKind::String ? FSoftClassPath(ClassValue->StringValue).ResolveClass() : nullptr;
  const auto* Tree = Owners.Num() == 1 ? Owners[0]->GetTypedOuter<UBehaviorTree>() : nullptr;
  if (!HasAssetDocumentAIBehaviorTreeExactFields(Value, {TEXT("Class"), TEXT("Properties")}) || !Properties || Properties->Kind != EKind::Object
   || !Property || !Property->HasAnyPropertyFlags(CPF_InstancedReference) || !Class || Class->GetPathName() != ClassValue->StringValue
   || !Class->IsChildOf(Property->PropertyClass) || !Class->HasAnyClassFlags(CLASS_EditInlineNew)
   || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
   || !Tree || Tree->BTGraph != nullptr || !Tree->HasAnyFlags(RF_Transient) || Tree->IsAsset()
   || (Class->ClassWithin && !Owners[0]->IsA(Class->ClassWithin))) return false;
  PropertyCustomizationHelpers::CreateNewInstanceOfEditInlineObjectClass(Handle.AsShared(), Class, Flags);
  UObject* Instance = nullptr;
  if (Handle.GetValue(Instance) != FPropertyAccess::Success || !Instance || Instance->GetClass() != Class || Instance->GetOuter() != Owners[0]) return false;
  FString Error;
  return WriteProperties(*Instance, *Properties, Error);
 }
 const auto* Kind = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Kind"));
 const auto* Path = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Path"));
 if (!HasAssetDocumentAIBehaviorTreeExactFields(Value, {TEXT("Kind"), TEXT("Path")})
  || !Kind || Kind->Kind != EKind::String || !Path || Path->Kind != EKind::String) return false;
 const FProperty* Property = Handle.GetProperty();
 const bool bClass = CastField<FClassProperty>(Property) || CastField<FSoftClassProperty>(Property);
 if (Kind->StringValue != (bClass ? TEXT("ClassRef") : TEXT("AssetRef"))) return false;
 const FSoftObjectPath ObjectPath(Path->StringValue);
 if (!ObjectPath.IsValid() || ObjectPath.ToString() != Path->StringValue) return false;
 if (CastField<FSoftObjectProperty>(Property))
  return Handle.SetValueFromFormattedString(Path->StringValue, Flags) == FPropertyAccess::Success;
 UObject* Object = ObjectPath.ResolveObject();
 if (!Object || Object->GetPathName() != Path->StringValue || (bClass && !Cast<UClass>(Object))) return false;
 return Handle.SetValue(Object, Flags) == FPropertyAccess::Success;
}

bool WriteValue(IPropertyHandle& Handle, const FValue& Value, int32 Depth, bool bContainerElement)
{
 if (Depth > 64 || !Handle.IsValidHandle() || Handle.IsEditConst()) return false;
 const FProperty* Property = Handle.GetProperty();
 if (!Property || (!bContainerElement && !Readback::IsAuthoredProperty(Handle))
  || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst)) return false;
 if (Handle.AsArray().IsValid() || Handle.AsSet().IsValid() || Handle.AsMap().IsValid())
  return WriteContainer(Handle, Value, Depth);
 if (const auto* Struct = CastField<FStructProperty>(Property))
 {
  if (Struct->Struct == FInstancedStruct::StaticStruct())
  {
   if (Value.Kind == EKind::Null)
    return Handle.SetValueFromFormattedString(TEXT("None"), Flags) == FPropertyAccess::Success;
   const auto* Type = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Struct"));
   const auto* Properties = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Properties"));
   if (!HasAssetDocumentAIBehaviorTreeExactFields(Value, {TEXT("Properties"), TEXT("Struct")})
    || !Type || Type->Kind != EKind::String || !Properties || Properties->Kind != EKind::Object) return false;
   const FSoftObjectPath Path(Type->StringValue);
   const auto* ScriptStruct = Cast<UScriptStruct>(Path.ResolveObject());
   if (!ScriptStruct || ScriptStruct->GetPathName() != Type->StringValue
    || Handle.SetValueFromFormattedString(Type->StringValue + TEXT("()"), Flags) != FPropertyAccess::Success) return false;
   // The public struct customization re-generates its typed structure scope
   // after SetValue. It owns allocation and type switching, never this provider.
   uint32 Count = 0;
   if (Handle.GetNumChildren(Count) != FPropertyAccess::Success || Count > 4096) return false;
   TSharedPtr<IPropertyHandle> Structure;
   for (uint32 Index = 0; Index < Count; ++Index)
   {
    auto Child = Handle.GetChildHandle(Index);
    if (!Child.IsValid()) return false;
    TArray<TSharedPtr<FStructOnScope>> Scopes;
    Child->GetOuterStructs(Scopes);
    if (Scopes.Num() == 1 && Scopes[0].IsValid() && Scopes[0]->GetStruct() == ScriptStruct)
    { if (Structure.IsValid()) return false; Structure = Child; }
   }
   if (!Structure.IsValid()) return false;
   for (int32 Index = 0; Index < Properties->ObjectKeys.Num(); ++Index)
   {
    auto Child = Structure->GetChildHandle(FName(*Properties->ObjectKeys[Index]), false);
    if (!Child.IsValid() || !Child->GetProperty() || Child->GetProperty()->GetName() != Properties->ObjectKeys[Index]
     || !WriteValue(*Child, Properties->Elements[Index], Depth + 1)) return false;
   }
   return true;
  }
  if (Value.Kind != EKind::Object) return false;
  if (Struct->Struct == FBlackboardKeySelector::StaticStruct())
  {
   const auto* Key = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("Key"));
   auto Child = Handle.GetChildHandle(TEXT("SelectedKeyName"), false);
   return HasAssetDocumentAIBehaviorTreeExactFields(Value, {TEXT("Key")}) && Key && Key->Kind == EKind::String
    && Child.IsValid() && Child->SetValue(FName(*Key->StringValue), Flags) == FPropertyAccess::Success;
  }
  for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
  {
   auto Child = Handle.GetChildHandle(FName(*Value.ObjectKeys[Index]), false);
   if (!Child.IsValid() || !Child->GetProperty() || Child->GetProperty()->GetName() != Value.ObjectKeys[Index]
    || !WriteValue(*Child, Value.Elements[Index], Depth + 1)) return false;
  }
  return true;
 }
 const UEnum* Enum = nullptr;
 if (const auto* E = CastField<FEnumProperty>(Property)) Enum = E->GetEnum();
 if (const auto* E = CastField<FByteProperty>(Property)) Enum = E->Enum;
 if (Enum)
 {
  int32 Index = Value.Kind == EKind::Integer ? Enum->GetIndexByValue(Value.IntegerValue)
   : (Value.Kind == EKind::String ? Enum->GetIndexByNameString(Value.StringValue) : INDEX_NONE);
  if (Index == INDEX_NONE) return false;
  if (Value.Kind == EKind::String && Value.StringValue != Enum->GetNameStringByIndex(Index)
   && Value.StringValue != Enum->GetNameByIndex(Index).ToString()) return false;
  return Handle.SetValueFromFormattedString(Enum->GetNameStringByIndex(Index), Flags) == FPropertyAccess::Success;
 }
 if (CastField<FBoolProperty>(Property)) return Value.Kind == EKind::Boolean && Handle.SetValue(Value.BooleanValue, Flags) == FPropertyAccess::Success;
 if (CastField<FInt8Property>(Property)) return SetInteger<int8>(Handle, Value);
 if (CastField<FInt16Property>(Property)) return SetInteger<int16>(Handle, Value);
 if (CastField<FIntProperty>(Property)) return SetInteger<int32>(Handle, Value);
 if (CastField<FInt64Property>(Property)) return SetInteger<int64>(Handle, Value);
 if (CastField<FByteProperty>(Property)) return SetInteger<uint8>(Handle, Value);
 if (CastField<FUInt16Property>(Property)) return SetInteger<uint16>(Handle, Value);
 if (CastField<FUInt32Property>(Property)) return SetInteger<uint32>(Handle, Value);
 if (CastField<FUInt64Property>(Property)) return SetInteger<uint64>(Handle, Value);
 if (CastField<FFloatProperty>(Property) || CastField<FDoubleProperty>(Property))
 {
  if (Value.Kind != EKind::Number && Value.Kind != EKind::Integer) return false;
  double Number = Value.Kind == EKind::Number ? Value.NumberValue : static_cast<double>(Value.IntegerValue);
  if (!FMath::IsFinite(Number)) return false;
  if (CastField<FFloatProperty>(Property)) return FMath::IsFinite(static_cast<float>(Number))
   && Handle.SetValue(static_cast<float>(Number), Flags) == FPropertyAccess::Success;
  return Handle.SetValue(Number, Flags) == FPropertyAccess::Success;
 }
 if (CastField<FObjectProperty>(Property) || CastField<FSoftObjectProperty>(Property)) return WriteReference(Handle, Value);
 if (Value.Kind != EKind::String) return false;
 if (CastField<FStrProperty>(Property)) return Handle.SetValue(Value.StringValue, Flags) == FPropertyAccess::Success;
 if (CastField<FNameProperty>(Property)) return Handle.SetValue(FName(*Value.StringValue), Flags) == FPropertyAccess::Success;
 if (CastField<FTextProperty>(Property)) return Handle.SetValue(FText::FromString(Value.StringValue), Flags) == FPropertyAccess::Success;
 return false;
}
}

bool WriteIsolatedValueOrProperties(const TSharedRef<FStructOnScope>& Storage,
 const FValue& Properties, FString& Error)
{
 if (!ExactValueOrType(Storage->GetStruct()) || !Storage->GetStructMemory()
  || Properties.Kind != EKind::Object || ContainsInlineObject(Properties))
 { Error = TEXT("An exact independently initialized ValueOr leaf without inline-object writes is required"); return false; }
 auto* Module = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
 if (!Module) { Error = TEXT("PropertyEditor unavailable"); return false; }
 TSharedPtr<IPropertyRowGenerator> Rows = Module->CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
 if (!Rows.IsValid()) { Error = TEXT("PropertyEditor rows unavailable"); return false; }
 Readback::ConfigureReadOnlyValueOrRows(*Rows);
 Rows->SetStructure(Storage);
 TMap<FString, TSharedPtr<IPropertyHandle>> Fields;
 int32 Budget = 4096;
 TFunction<bool(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather;
 Gather = [&](const auto& Nodes)
 {
  for (const auto& Node : Nodes)
  {
   if (--Budget < 0) return false;
   auto Handle = Node->CreatePropertyHandle();
   for (int32 Depth = 0; Handle.IsValid() && Depth < 64; ++Depth, Handle = Handle->GetParentHandle())
   {
    const FProperty* Property = Handle->GetProperty();
    if (Property && Property->GetOwnerProperty() == Property
     && Storage->GetStruct()->IsChildOf(Property->GetOwnerStruct())
     && Readback::IsAuthoredProperty(*Handle)) Fields.Add(Property->GetName(), Handle);
   }
   TArray<TSharedRef<IDetailTreeNode>> Children;
   Node->GetChildren(Children, true);
   if (!Gather(Children)) return false;
  }
  return true;
 };
 if (!Gather(Rows->GetRootTreeNodes())) { Error = TEXT("ValueOr property row discovery failed"); return false; }
 for (int32 Index = 0; Index < Properties.ObjectKeys.Num(); ++Index)
 {
  const auto* Handle = Fields.Find(Properties.ObjectKeys[Index]);
  if (!Handle || !Handle->IsValid() || !WriteValue(**Handle, Properties.Elements[Index], 0))
  { Error = Properties.ObjectKeys[Index] + TEXT(": isolated typed ValueOr write rejected"); return false; }
 }
 return true;
}

bool WriteProperties(UObject& Object, const FValue& Properties, FString& Error)
{
 if (Properties.Kind != EKind::Object) { Error = TEXT("authored_properties must be an object"); return false; }
 if (Properties.ObjectKeys.IsEmpty()) return true;
 TSet<FString> Fields;
 for (const auto& Key : Properties.ObjectKeys) Fields.Add(Key);
 TMap<FName, TSharedPtr<IPropertyHandle>> Handles;
 auto Rows = RowsFor(Object, false, Fields, Handles);
 if (!Rows.IsValid()) { Error = TEXT("PropertyEditor rows unavailable"); return false; }
 TArray<Readback::FHiddenCategoryScalarHandle> HiddenScalars;
 for (int32 Index = 0; Index < Properties.ObjectKeys.Num(); ++Index)
 {
  const FName Name(*Properties.ObjectKeys[Index]);
  if (!Handles.Contains(Name))
   if (const auto* Property = FindFProperty<FProperty>(Object.GetClass(), Name);
    Property && Property->GetName() == Properties.ObjectKeys[Index]
     && Readback::IsHiddenCategoryAuthoredScalar(Object, *Property))
   {
    Readback::FHiddenCategoryScalarHandle Scalar;
    if (!Readback::MakeHiddenCategoryScalarHandle(Object, *Property, Scalar))
    { Error = Object.GetClass()->GetPathName() + TEXT(".") + Properties.ObjectKeys[Index] + TEXT(": hidden-category scalar handle unavailable"); return false; }
    Handles.Add(Name, Scalar.Handle);
    HiddenScalars.Add(MoveTemp(Scalar));
   }
  const auto* Handle = Handles.Find(FName(*Properties.ObjectKeys[Index]));
  if (!Handle || !Handle->IsValid() || !(*Handle)->GetProperty() || (*Handle)->GetProperty()->GetName() != Properties.ObjectKeys[Index]
   || !Readback::IsAuthoredProperty(**Handle))
  { Error = Object.GetClass()->GetPathName() + TEXT(".") + Properties.ObjectKeys[Index] + TEXT(": typed property write unavailable or invalid"); return false; }
  if ((*Handle)->IsEditConst())
  {
   // Complete defaults include fields disabled by their stock edit condition.
   // Preserve an exactly equal typed value without invoking any setter.
   if (IsUnchangedDisabledAuthoredValue(Object, **Handle, Properties.Elements[Index])) continue;
   Error = Object.GetClass()->GetPathName() + TEXT(".") + Properties.ObjectKeys[Index] + TEXT(": disabled authored field differs from its current typed value"); return false;
  }
  if (!WriteValue(**Handle, Properties.Elements[Index], 0))
  { Error = Object.GetClass()->GetPathName() + TEXT(".") + Properties.ObjectKeys[Index] + TEXT(": typed property write unavailable or invalid"); return false; }
 }
 FValue Actual;
 if (!Readback::BuildAuthoredProperties(Object, Actual, nullptr, Readback::EAuthoredGraphReadbackVersion::V2)) { Error = TEXT("Typed authored readback failed after property write"); return false; }
 return true;
}

bool WriteExactHiddenField(UObject& Object, FName Field, const FString& Formatted, FString& Error)
{
 // This exact finite initialization surface is scratch-only at its call site.
 // The public generator exposes hidden fields; no property-memory write is used.
 const TSet<FName> NodeFields = {TEXT("NodeGuid"), TEXT("NodePosX"), TEXT("NodePosY"), TEXT("NodeComment"),
  TEXT("bCommentBubblePinned"), TEXT("bCommentBubbleVisible")};
 const TSet<FName> CommentFields = {TEXT("NodeWidth"), TEXT("NodeHeight"), TEXT("CommentDepth"), TEXT("FontSize"),
  TEXT("MoveMode"), TEXT("NodeDetails"), TEXT("bCommentBubbleVisible_InDetailsPanel"), TEXT("bColorCommentBubble"), TEXT("CommentColor")};
 if (!((Cast<UEdGraph>(&Object) && Field == TEXT("GraphGuid"))
  || (Cast<UEdGraphNode>(&Object) && NodeFields.Contains(Field))
  || (Cast<UEdGraphNode_Comment>(&Object) && CommentFields.Contains(Field))))
 { Error = TEXT("Hidden field outside exact scratch initialization surface"); return false; }
 TMap<FName, TSharedPtr<IPropertyHandle>> Handles;
 auto Rows = RowsFor(Object, true, {Field.ToString()}, Handles);
 const auto* Handle = Handles.Find(Field);
 if (!Rows.IsValid() || !Handle || !Handle->IsValid())
 { Error = Object.GetClass()->GetPathName() + TEXT(".") + Field.ToString() + TEXT(": exact hidden PropertyEditor handle unavailable"); return false; }
 if ((*Handle)->IsEditConst())
 { Error = Object.GetClass()->GetPathName() + TEXT(".") + Field.ToString() + TEXT(": exact hidden PropertyEditor handle is edit-const or its edit condition is false"); return false; }
 const FProperty* Property = (*Handle)->GetProperty();
 const auto Result = CastField<FStrProperty>(Property) ? (*Handle)->SetValue(Formatted, Flags)
  : (CastField<FTextProperty>(Property) ? (*Handle)->SetValue(FText::FromString(Formatted), Flags)
   : (*Handle)->SetValueFromFormattedString(Formatted, Flags));
 if (Result != FPropertyAccess::Success)
 { Error = Object.GetClass()->GetPathName() + TEXT(".") + Field.ToString() + TEXT(": exact hidden PropertyEditor write rejected"); return false; }
 return true;
}
}
