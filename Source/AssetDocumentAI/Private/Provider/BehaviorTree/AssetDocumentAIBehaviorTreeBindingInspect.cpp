// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeBindingInspect.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeScratchAuthoring.h"
#include "TypedPreimage/AssetDocumentAISelectorTypedPreimageInternal.h"
#include "AssetDocumentEditorNativeState.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Tasks/BTTask_RunBehaviorDynamic.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "BehaviorTreeGraph.h"
#include "DetailWidgetRow.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "IPropertyTypeCustomization.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace AssetDocumentAIBehaviorTreeBindingInspect
{
using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
static bool bInspecting = false; // Game Thread only; no cross-request state survives.

FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const FString& Message)
{ return MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, *Message); }
const FValue* Field(const FValue& V, const TCHAR* Name)
{
 const int32 I = V.ObjectKeys.IndexOfByKey(Name);
 return V.Kind == EKind::Object && V.Elements.IsValidIndex(I) ? &V.Elements[I] : nullptr;
}
FValue* Field(FValue& V, const TCHAR* Name)
{
 const int32 I = V.ObjectKeys.IndexOfByKey(Name);
 return V.Kind == EKind::Object && V.Elements.IsValidIndex(I) ? &V.Elements[I] : nullptr;
}
bool Fields(const FValue& V, TArray<FString> Names)
{ return V.Kind == EKind::Object && V.ObjectKeys == Names && V.Elements.Num() == Names.Num(); }
bool Path(const FValue& V)
{
 if (V.Kind != EKind::String || V.StringValue.IsEmpty()) return false;
 const FSoftObjectPath P(V.StringValue);
 return P.IsValid() && P.GetSubPathString().IsEmpty() && P.ToString() == V.StringValue;
}
bool NullablePath(const FValue& V) { return V.Kind == EKind::Null || Path(V); }
bool Authored(const FProperty& P)
{ return P.HasAnyPropertyFlags(CPF_Edit) && !P.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst); }
bool NodeFamily(const UClass& C)
{
 return C.IsChildOf(UBTTaskNode::StaticClass()) || C.IsChildOf(UBTCompositeNode::StaticClass())
  || C.IsChildOf(UBTDecorator::StaticClass()) || C.IsChildOf(UBTService::StaticClass());
}
bool Concrete(const UClass& C)
{ return !C.HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists); }
bool Equal(const FValue& A, const FValue& B)
{
 TArray<uint8> AB, BB;
 return EncodeAuthoredCanonicalValue(A, AB) && EncodeAuthoredCanonicalValue(B, BB) && AB == BB;
}

// The stock selector widget can repair its selection during row construction.
// This instance-specific passive layout exposes the same property handles.
class FPassiveSelector final : public IPropertyTypeCustomization
{
 void CustomizeHeader(TSharedRef<IPropertyHandle> Handle, FDetailWidgetRow& Row, IPropertyTypeCustomizationUtils&) override
 { Row.NameContent()[Handle->CreatePropertyNameWidget()].ValueContent()[Handle->CreatePropertyValueWidget()]; }
 void CustomizeChildren(TSharedRef<IPropertyHandle>, IDetailChildrenBuilder&, IPropertyTypeCustomizationUtils&) override {}
};

struct FCursor
{
 const UStruct* Type = nullptr;
 const FProperty* Property = nullptr;
 TSharedPtr<IPropertyHandle> Handle;
 TSharedPtr<FStructOnScope> Storage;
 const uint8* LiveStructMemory = nullptr; // Borrowed from the exact live FInstancedStruct; never an owned default.
 TMap<FString, TSharedPtr<IPropertyHandle>> Roots;
 bool bElement = false;
};
struct FLocatorContext
{
 UObject& Root;
 bool bDefaults;
 TArray<TSharedPtr<IPropertyRowGenerator>> Rows;
 TArray<TSharedPtr<FStructOnScope>> Scopes;
 int32 Remaining = 4096;
 FString LastFailure;

 bool Gather(const TArray<TSharedRef<IDetailTreeNode>>& Nodes, const UStruct& Type,
  TMap<FString, TSharedPtr<IPropertyHandle>>& Out)
 {
  for (const auto& Node : Nodes)
  {
   if (--Remaining < 0) return false;
   auto H = Node->CreatePropertyHandle();
   for (int32 Depth = 0; H.IsValid() && Depth < 64; ++Depth, H = H->GetParentHandle())
   {
    const FProperty* P = H->GetProperty();
    if (P && P->GetOwnerProperty() == P && Type.IsChildOf(P->GetOwnerStruct()) && Authored(*P))
     Out.Add(P->GetName(), H);
   }
   TArray<TSharedRef<IDetailTreeNode>> Children;
   Node->GetChildren(Children, true);
   if (!Gather(Children, Type, Out)) return false;
  }
  return true;
 }
 TSharedPtr<IPropertyRowGenerator> MakeRows()
 {
  auto* Module = FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
  if (!Module) return nullptr;
  FPropertyRowGeneratorArgs Args;
  Args.bAllowEditingClassDefaultObjects = false;
  TSharedPtr<IPropertyRowGenerator> Generator = Module->CreatePropertyRowGenerator(Args);
  if (!Generator.IsValid()) return nullptr;
  ConfigureReadOnlyValueOrRows(*Generator);
  Generator->RegisterInstancedCustomPropertyTypeLayout(FBlackboardKeySelector::StaticStruct()->GetFName(),
   FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() -> TSharedRef<IPropertyTypeCustomization>
   { return MakeShared<FPassiveSelector>(); }));
  Rows.Add(Generator);
  return Generator;
 }
 bool Object(UObject& O, FCursor& Out)
 {
  auto Generator = MakeRows();
  if (!Generator.IsValid() || !IsValid(&O)) return false;
  Out = {}; Out.Type = O.GetClass();
  Generator->SetObjects({&O});
  return Gather(Generator->GetRootTreeNodes(), *Out.Type, Out.Roots);
 }
 bool Structure(const UScriptStruct& Type, FCursor& Out)
 {
  auto Generator = MakeRows();
  if (!Generator.IsValid()) return false;
  auto Storage = MakeShared<FStructOnScope>(&Type); // Initialize, never copy CDO/live storage.
  Scopes.Add(Storage); Generator->SetStructure(Storage);
  Out = {}; Out.Type = &Type; Out.Storage = Storage;
  return Gather(Generator->GetRootTreeNodes(), Type, Out.Roots);
 }
 bool Declared(const FProperty& P, FCursor& Out)
 {
  if (const auto* S = CastField<FStructProperty>(&P)) return S->Struct && Structure(*S->Struct, Out);
  if (const auto* O = CastField<FObjectPropertyBase>(&P); O && P.HasAnyPropertyFlags(CPF_InstancedReference))
  { Out = {}; Out.Property = &P; return true; }
  return false;
 }
 bool SetHandle(const TSharedPtr<IPropertyHandle>& H, FCursor& Out, bool bElement = false)
 {
  if (!H.IsValid() || !H->IsValidHandle() || !H->GetProperty()) return false;
  Out = {}; Out.Handle = H; Out.Property = H->GetProperty(); Out.bElement = bElement;
  return true;
 }
 bool Member(FCursor& Cursor, const FString& Name)
 {
  if (Name.IsEmpty()) return false;
  const UStruct* Type = Cursor.Type;
  if (!Type)
  {
   const auto* S = CastField<FStructProperty>(Cursor.Property);
   if (!S || (!Cursor.bElement && Cursor.Property->ArrayDim > 1) || S->Struct == FInstancedStruct::StaticStruct()) return false;
   Type = S->Struct;
  }
  const auto* P = FindFProperty<FProperty>(Type, *Name);
  if (!P || P->GetName() != Name || !Authored(*P)) return false;
  const auto* RootHandle = Cursor.Roots.Find(Name);
  const auto Child = RootHandle ? *RootHandle : (Cursor.Handle.IsValid() ? Cursor.Handle->GetChildHandle(FName(*Name), false) : nullptr);
  if (!Child.IsValid() || Child->GetProperty() != P) return false;
  if (Cursor.LiveStructMemory)
  {
   void* Address = nullptr;
   if (Child->GetValueData(Address) != FPropertyAccess::Success
    || Address != P->ContainerPtrToValuePtr<void>(Cursor.LiveStructMemory)) return false;
  }
  return SetHandle(Child, Cursor);
 }
 bool Array(FCursor& Cursor, int64 Index)
 {
  const FProperty* P = Cursor.Property;
  const auto* A = CastField<FArrayProperty>(P);
  if (!P || Index < 0 || Index > MAX_int32 || (!A && P->ArrayDim <= 1) || Cursor.bElement) return false;
  if (!A && Index >= P->ArrayDim) return false;
  if (bDefaults) return Declared(A ? *A->Inner : *P, Cursor);
  auto ArrayHandle = Cursor.Handle.IsValid() ? Cursor.Handle->AsArray() : nullptr;
  uint32 Count = 0;
  return ArrayHandle.IsValid() && ArrayHandle->GetNumElements(Count) == FPropertyAccess::Success
   && Count <= 4096 && Index < Count && SetHandle(ArrayHandle->GetElement(static_cast<uint32>(Index)), Cursor, true);
 }
 bool Read(IPropertyHandle& H, FValue& Out)
 {
  FPropertyReadContext C; C.Version = EAuthoredGraphReadbackVersion::V2; C.RootObject = &Root;
  return ReadAuthoredProperty(H, C, 0, Out);
 }
 bool MapKey(const FProperty& P, const FValue& Desired, const FValue& Actual)
 {
  if (Equal(Desired, Actual)) return true;
  if (Desired.Kind != EKind::String || Actual.Kind != EKind::Integer) return false;
  int64 Number = 0;
  if (LexTryParseString(Number, *Desired.StringValue) && LexToString(Number) == Desired.StringValue)
   return Number == Actual.IntegerValue;
  const UEnum* Enum = nullptr;
  if (const auto* E = CastField<FEnumProperty>(&P)) Enum = E->GetEnum();
  if (const auto* E = CastField<FByteProperty>(&P)) Enum = E->Enum;
  const int32 I = Enum ? Enum->GetIndexByNameString(Desired.StringValue) : INDEX_NONE;
  return I != INDEX_NONE && (Enum->GetNameStringByIndex(I) == Desired.StringValue || Enum->GetNameByIndex(I).ToString() == Desired.StringValue)
   && Enum->GetValueByIndex(I) == Actual.IntegerValue;
 }
 bool Map(FCursor& Cursor, const FValue& Key)
 {
  const auto* P = CastField<FMapProperty>(Cursor.Property);
  if (!P || (!CastField<FStrProperty>(P->KeyProp) && !CastField<FNameProperty>(P->KeyProp)
   && !CastField<FByteProperty>(P->KeyProp) && !CastField<FEnumProperty>(P->KeyProp))) return false;
  if (Key.Kind != EKind::String && Key.Kind != EKind::Integer) return false;
  if (bDefaults) return Declared(*P->ValueProp, Cursor);
  auto MapHandle = Cursor.Handle.IsValid() ? Cursor.Handle->AsMap() : nullptr;
  uint32 Count = 0;
  if (!MapHandle.IsValid() || MapHandle->GetNumElements(Count) != FPropertyAccess::Success || Count > 4096) return false;
  TSharedPtr<IPropertyHandle> Match;
  for (uint32 I = 0; I < Count; ++I)
  {
   const auto Element = MapHandle->GetElement(I); const auto KH = Element->GetKeyHandle(); FValue Actual;
   if (!KH.IsValid() || !Read(*KH, Actual)) return false;
   if (MapKey(*P->KeyProp, Key, Actual)) { if (Match.IsValid()) return false; Match = Element; }
  }
  return SetHandle(Match, Cursor, true);
 }
 bool Set(FCursor& Cursor, const FValue& Value)
 {
  const auto* P = CastField<FSetProperty>(Cursor.Property);
  if (!P) return false;
  if (bDefaults) return Declared(*P->ElementProp, Cursor);
  auto SetHandleValue = Cursor.Handle.IsValid() ? Cursor.Handle->AsSet() : nullptr;
  uint32 Count = 0;
  if (!SetHandleValue.IsValid() || SetHandleValue->GetNumElements(Count) != FPropertyAccess::Success || Count > 4096) return false;
  TSharedPtr<IPropertyHandle> Match;
  for (uint32 I = 0; I < Count; ++I)
  {
   const auto Element = SetHandleValue->GetElement(I); FValue Actual;
   if (!Read(*Element, Actual)) return false;
   if (Equal(Value, Actual)) { if (Match.IsValid()) return false; Match = Element; }
  }
  return SetHandle(Match, Cursor, true);
 }
 bool Inline(FCursor& Cursor, const FString& ClassPath)
 {
  const auto* P = CastField<FObjectPropertyBase>(Cursor.Property);
  UClass* Class = FSoftClassPath(ClassPath).ResolveClass();
  if (!P || !P->HasAnyPropertyFlags(CPF_InstancedReference) || !Class || Class->GetPathName() != ClassPath
   || !Class->IsChildOf(P->PropertyClass) || !Class->HasAnyClassFlags(CLASS_EditInlineNew) || !Concrete(*Class)) return false;
  UObject* Target = bDefaults ? Class->GetDefaultObject(false) : nullptr;
  if (!bDefaults && (!Cursor.Handle.IsValid() || Cursor.Handle->GetValue(Target) != FPropertyAccess::Success
   || !Target || Target->GetClass() != Class || !Target->IsIn(&Root))) return false;
  return Target && Object(*Target, Cursor);
 }
 bool Dynamic(FCursor& Cursor, const FString& StructPath)
 {
  const auto* P = CastField<FStructProperty>(Cursor.Property);
  const auto* Type = Cast<UScriptStruct>(FSoftObjectPath(StructPath).ResolveObject());
  if (!P || (!Cursor.bElement && P->ArrayDim != 1) || P->Struct != FInstancedStruct::StaticStruct()
   || !Type || Type->GetPathName() != StructPath) return false;
  // Explicit types must be reachable through this FInstancedStruct declaration.
  if (P->HasMetaData(TEXT("BaseStruct")))
  {
   const FString BasePath = P->GetMetaData(TEXT("BaseStruct"));
   const auto* Base = Cast<UScriptStruct>(FSoftObjectPath(BasePath).ResolveObject());
   if (!Base || !Type->IsChildOf(Base)) return false;
  }
  if (bDefaults) return Structure(*Type, Cursor);
  void* Address = nullptr;
  if (!Cursor.Handle.IsValid() || Cursor.Handle->GetValueData(Address) != FPropertyAccess::Success || !Address) return false;
  const auto* Instance = static_cast<const FInstancedStruct*>(Address);
  if (!Instance->IsValid() || Instance->GetScriptStruct() != Type) return false;
  const uint8* Memory = Instance->GetMemory();
  uint32 Count = 0;
  if (Cursor.Handle->GetNumChildren(Count) != FPropertyAccess::Success || Count > 4096) return false;
  TSharedPtr<IPropertyHandle> Match;
  for (uint32 I = 0; I < Count; ++I)
  {
   const auto Child = Cursor.Handle->GetChildHandle(I);
   TArray<TSharedPtr<FStructOnScope>> ChildScopes;
   if (!Child.IsValid()) return false;
   // Fields share their wrapper's outer scope; only the actual structure
   // wrapper may supply field handles for this typed live instance.
   const auto* Wrapper = CastField<FStructProperty>(Child->GetProperty());
   if ((!Child->AsStruct().IsValid() && !Wrapper) || (Wrapper && Wrapper->Struct != Type)) continue;
   Child->GetOuterStructs(ChildScopes);
   if (ChildScopes.Num() == 1 && ChildScopes[0].IsValid() && ChildScopes[0]->GetStruct() == Type
    && ChildScopes[0]->GetStructMemory() == Memory)
   { if (Match.IsValid()) return false; Match = Child; }
  }
  if (!Match.IsValid()) return false;
  Cursor = {}; Cursor.Type = Type; Cursor.Handle = Match; Cursor.LiveStructMemory = Memory;
  return true;
 }
 bool Resolve(const FCursor& Start, const FValue& Locator, FCursor& Out)
 {
  LastFailure = TEXT("Locator requires one to 64 typed steps.");
  if (Locator.Kind != EKind::Array || Locator.Elements.IsEmpty() || Locator.Elements.Num() > 64) return false;
  Out = Start;
  for (int32 Index = 0; Index < Locator.Elements.Num(); ++Index)
  {
   const auto& Step = Locator.Elements[Index];
   LastFailure = FString::Printf(TEXT("Locator step %d has no exact kind."), Index);
   const auto* K = Field(Step, TEXT("kind"));
   if (!K || K->Kind != EKind::String) return false;
   const FString& Kind = K->StringValue;
   const auto* Name = Field(Step, TEXT("name"));
   LastFailure = FString::Printf(TEXT("Locator step %d kind=%s%s is unreachable from %s."), Index, *Kind,
    Name && Name->Kind == EKind::String ? *FString::Printf(TEXT(" field=%s"), *Name->StringValue) : TEXT(""),
    Out.Property ? *Out.Property->GetName() : Out.Type ? *Out.Type->GetPathName() : TEXT("unbound storage"));
   if (Kind == TEXT("field"))
   { if (!Fields(Step, {TEXT("kind"), TEXT("name")}) || Step.Elements[1].Kind != EKind::String || !Member(Out, Step.Elements[1].StringValue)) return false; }
   else if (Kind == TEXT("array_element"))
   { if (!Fields(Step, {TEXT("index"), TEXT("kind")}) || Step.Elements[0].Kind != EKind::Integer || !Array(Out, Step.Elements[0].IntegerValue)) return false; }
   else if (Kind == TEXT("map_value"))
   { if (!Fields(Step, {TEXT("key"), TEXT("kind")}) || !Map(Out, Step.Elements[0])) return false; }
   else if (Kind == TEXT("set_element"))
   { if (!Fields(Step, {TEXT("kind"), TEXT("value")}) || !Set(Out, Step.Elements[1])) return false; }
   else if (Kind == TEXT("instanced_object"))
   { if (!Fields(Step, {TEXT("class_path"), TEXT("kind")}) || !Path(Step.Elements[0]) || !Inline(Out, Step.Elements[0].StringValue)) return false; }
   else if (Kind == TEXT("instanced_struct"))
   { if (!Fields(Step, {TEXT("kind"), TEXT("struct_path")}) || !Path(Step.Elements[1]) || !Dynamic(Out, Step.Elements[1].StringValue)) return false; }
   else return false;
  }
  LastFailure.Reset();
  return true;
 }
};

const UScriptStruct* LeafType(const FCursor& C)
{
 if (C.LiveStructMemory || C.Storage.IsValid()) return Cast<UScriptStruct>(C.Type);
 const auto* S = CastField<FStructProperty>(C.Property);
 return S && (C.bElement || C.Property->ArrayDim == 1) ? S->Struct.Get() : nullptr;
}
const void* LeafAddress(const FCursor& C)
{
 if (C.Storage.IsValid()) return C.Storage->GetStructMemory();
 if (C.LiveStructMemory) return C.LiveStructMemory;
 void* Address = nullptr;
 return C.Handle.IsValid() && C.Handle->GetValueData(Address) == FPropertyAccess::Success ? Address : nullptr;
}
const FBlackboardEntry* Entry(const UBlackboardData* BB, const FString& Key)
{
 const FName Name(*Key);
 const FBlackboard::FKey Id = BB && !Name.IsNone() ? BB->GetKeyID(Name) : FBlackboard::InvalidKey;
 return BB && Id != FBlackboard::InvalidKey ? BB->GetKey(Id) : nullptr;
}
bool EnumEvidence(const UBlackboardKeyType* KeyType, FValue& Out)
{
 const UEnum* Enum = nullptr;
 if (const auto* E = Cast<UBlackboardKeyType_Enum>(KeyType)) Enum = E->EnumType;
 if (const auto* E = Cast<UBlackboardKeyType_NativeEnum>(KeyType)) Enum = E->EnumType;
 Out = NullValue();
 if (!Enum) return true;
 TMap<FString, int64> Tokens;
 for (int32 I = 0; I < Enum->NumEnums(); ++I)
 {
  bool bHasToken = false;
  for (const FString& Token : {Enum->GetNameStringByIndex(I), Enum->GetNameByIndex(I).ToString()})
  {
   if (Token.IsEmpty()) continue;
   if (Tokens.Contains(Token) && Tokens.FindChecked(Token) != Enum->GetValueByIndex(I)) return false;
   Tokens.Add(Token, Enum->GetValueByIndex(I));
   bHasToken = true;
  }
  if (!bHasToken) return false;
 }
 TArray<FString> Names; Tokens.GetKeys(Names); Names.Sort(); TArray<FValue> Values;
 for (const FString& Name : Names) Values.Add(IntegerValue(Tokens.FindChecked(Name)));
 Out = ObjectValue(MoveTemp(Names), MoveTemp(Values)); return true;
}
bool SelectorEvidence(const FCursor& C, const UBlackboardData* BB, const FString& Key,
 bool bDecoratorSelector, FValue& Out)
{
 if (LeafType(C) != FBlackboardKeySelector::StaticStruct()) return false;
 const auto* Selector = static_cast<const FBlackboardKeySelector*>(LeafAddress(C));
 const auto* NoneField = FindFProperty<FBoolProperty>(FBlackboardKeySelector::StaticStruct(), TEXT("bNoneIsAllowedValue"));
 if (!Selector || !NoneField) return false;
 const auto* Found = Entry(BB, Key);
 bool Passes = Found && Found->KeyType && Selector->AllowedTypes.IsEmpty();
 if (Found && Found->KeyType)
  for (const auto& Filter : Selector->AllowedTypes)
   if (Filter && Found->KeyType->IsAllowedByFilter(Filter)) Passes = true;
 FValue EnumValues;
 if (!EnumEvidence(Found ? Found->KeyType.Get() : nullptr, EnumValues)) return false;
 Out = ObjectValue({TEXT("blackboard_decorator_selector"), TEXT("enum_values_or_null"), TEXT("filter_passes"), TEXT("key_exists"), TEXT("none_allowed")},
  {BooleanValue(bDecoratorSelector), MoveTemp(EnumValues), BooleanValue(Passes), BooleanValue(Found != nullptr), BooleanValue(NoneField->GetPropertyValue_InContainer(Selector))});
 return true;
}
bool ValueOrCompatible(const UScriptStruct& Type, const void* Address, const UBlackboardKeyType* Key, bool& Out)
{
 if (!Address) return false;
#define BT_BINDING_FAMILY(N) if (&Type == FValueOrBBKey_##N::StaticStruct()) { Out = static_cast<const FValueOrBBKey_##N*>(Address)->IsCompatibleType(Key); return true; }
 BT_BINDING_FAMILY(Bool) BT_BINDING_FAMILY(Class) BT_BINDING_FAMILY(Enum) BT_BINDING_FAMILY(Float)
 BT_BINDING_FAMILY(Int32) BT_BINDING_FAMILY(Name) BT_BINDING_FAMILY(String) BT_BINDING_FAMILY(Object)
 BT_BINDING_FAMILY(Rotator) BT_BINDING_FAMILY(Struct) BT_BINDING_FAMILY(Vector)
#undef BT_BINDING_FAMILY
 return false;
}
bool FullValueOrFields(const UScriptStruct& Type, const FValue& V)
{
 if (&Type == FValueOrBBKey_Class::StaticStruct() || &Type == FValueOrBBKey_Object::StaticStruct())
  return Fields(V, {TEXT("BaseClass"), TEXT("DefaultValue"), TEXT("Key")});
 if (&Type == FValueOrBBKey_Enum::StaticStruct())
  return Fields(V, {TEXT("DefaultValue"), TEXT("EnumType"), TEXT("Key"), TEXT("NativeEnumTypeName")});
 if (&Type == FValueOrBBKey_Struct::StaticStruct())
  return Fields(V, {TEXT("bCanEditDefaultValueType"), TEXT("DefaultValue"), TEXT("Key")});
 return Fields(V, {TEXT("DefaultValue"), TEXT("Key")});
}
bool ValueOrEvidence(const FCursor& C, const UBlackboardData* BB, const FValue& Query, FValue& Out, FString& Reason)
{
 const auto* Type = LeafType(C);
 if (!Type) { Reason = TEXT("ValueOr requires a single typed struct leaf."); return false; }
 const auto* AuthoredValue = Field(Query, TEXT("authored_value_or_null"));
 const auto* Found = Entry(BB, Field(Query, TEXT("selected_key"))->StringValue);
 bool Compatible = false;
 const void* Address = LeafAddress(C);
 TSharedPtr<FStructOnScope> Storage;
 if (AuthoredValue->Kind != EKind::Null)
 {
  if (!FullValueOrFields(*Type, *AuthoredValue) || !ValueOrCompatible(*Type, Address, nullptr, Compatible))
  { Reason = TEXT("The authored ValueOr fields or exact family are invalid: ") + Type->GetPathName(); return false; }
  FValue Parameters = *AuthoredValue;
  if (Type == FValueOrBBKey_Struct::StaticStruct())
  {
   auto* Literal = Field(Parameters, TEXT("DefaultValue"));
   if (Literal->Kind != EKind::Null)
   {
    if (!Fields(*Literal, {TEXT("Properties"), TEXT("Struct")}) || Literal->Elements[0].Kind != EKind::Object || !Path(Literal->Elements[1]))
    { Reason = TEXT("Struct compatibility requires an explicit canonical Struct type envelope."); return false; }
    // Stock Struct compatibility reads the type alone. Its authored literal is
    // validated in Python; no literal member or inline object is preview-written.
    Literal->Elements[0] = ObjectValue({}, {});
   }
  }
  Storage = MakeShared<FStructOnScope>(Type); // Authored params never inherit inline UObject references.
  if (!UE::AssetDocumentAI::BehaviorTreeScratchAuthoring::WriteIsolatedValueOrProperties(Storage.ToSharedRef(), Parameters, Reason)) return false;
  Address = Storage->GetStructMemory();
 }
 if (!ValueOrCompatible(*Type, Address, Found ? Found->KeyType.Get() : nullptr, Compatible))
 { Reason = TEXT("An existing exact eleven-family ValueOr address is required: ") + Type->GetPathName(); return false; }
 Out = ObjectValue({TEXT("compatible"), TEXT("key_exists")}, {BooleanValue(Compatible), BooleanValue(Found != nullptr)});
 return true;
}
bool BlackboardChain(const UBlackboardData* BB)
{
 TSet<const UBlackboardData*> Seen;
 for (const auto* Current = BB; Current; Current = Current->Parent)
 {
  if (!IsValid(Current) || Seen.Contains(Current) || Seen.Num() >= 4096) return false;
  Seen.Add(Current);
 }
 return true;
}
bool SubtreeEvidence(UObject& Node, const FCursor& C, const UBlackboardData* BB, const FValue& Query, FValue& Out)
{
 const FString& Kind = Field(Query, TEXT("kind"))->StringValue;
 const bool Static = Kind == TEXT("static_subtree");
 const UClass* ExpectedOwner = Static ? UBTTask_RunBehavior::StaticClass() : UBTTask_RunBehaviorDynamic::StaticClass();
 const TCHAR* Name = Static ? TEXT("BehaviorAsset") : TEXT("DefaultBehaviorAsset");
 const auto* P = CastField<FObjectPropertyBase>(C.Property);
 if (!Node.IsA(ExpectedOwner) || !P || P != FindFProperty<FProperty>(ExpectedOwner, Name)
  || P->PropertyClass != UBehaviorTree::StaticClass() || !C.Handle.IsValid()) return false;
 const auto* Ref = Field(Query, TEXT("subtree_path_or_null"));
 auto* Tree = Ref->Kind == EKind::Null ? nullptr : Cast<UBehaviorTree>(FSoftObjectPath(Ref->StringValue).ResolveObject());
 if (Tree && (Tree->GetClass() != UBehaviorTree::StaticClass() || Tree->GetPathName() != Ref->StringValue)) return false;
 const UBlackboardData* SubBB = Tree ? Tree->BlackboardAsset.Get() : nullptr;
 if (!BlackboardChain(SubBB)) return false;
 const bool Compatible = BB && SubBB && (BB == SubBB || BB->IsChildOf(*SubBB));
 Out = ObjectValue({TEXT("blackboard_path_or_null"), TEXT("effective_blackboard_compatible"), TEXT("exists"), TEXT("has_root_decorators")},
  {SubBB ? StringValue(SubBB->GetPathName()) : NullValue(), BooleanValue(Compatible), BooleanValue(Tree != nullptr),
   BooleanValue(Tree && !Tree->RootDecorators.IsEmpty())});
 return true;
}
bool QueryShape(const FValue& Query)
{
 const auto* Kind = Field(Query, TEXT("kind"));
 const auto* Locator = Field(Query, TEXT("locator"));
 if (!Kind || Kind->Kind != EKind::String || !Locator || Locator->Kind != EKind::Array || Locator->Elements.IsEmpty()) return false;
 if (Kind->StringValue == TEXT("selector"))
  return Fields(Query, {TEXT("kind"), TEXT("locator"), TEXT("selected_key")}) && Query.Elements[2].Kind == EKind::String;
 if (Kind->StringValue == TEXT("value_or"))
  return Fields(Query, {TEXT("authored_value_or_null"), TEXT("kind"), TEXT("locator"), TEXT("selected_key")})
   && (Query.Elements[0].Kind == EKind::Null || Query.Elements[0].Kind == EKind::Object) && Query.Elements[3].Kind == EKind::String;
 return (Kind->StringValue == TEXT("static_subtree") || Kind->StringValue == TEXT("dynamic_subtree"))
  && Fields(Query, {TEXT("kind"), TEXT("locator"), TEXT("subtree_path_or_null")}) && NullablePath(Query.Elements[2]);
}
bool RequestShape(const FValue& V)
{
 if (!IsAssetDocumentAICanonicalRuntimeValue(V) || !Fields(V, {TEXT("blackboard_path_or_null"), TEXT("queries"), TEXT("source")})
  || !NullablePath(V.Elements[0]) || V.Elements[1].Kind != EKind::Array) return false;
 const auto& S = V.Elements[2]; const auto* Kind = Field(S, TEXT("kind"));
 if (!Kind || Kind->Kind != EKind::String) return false;
 if (Kind->StringValue == TEXT("class_defaults"))
 { if (!Fields(S, {TEXT("kind"), TEXT("node_class_path")}) || !Path(S.Elements[1])) return false; }
 else if (Kind->StringValue == TEXT("live_node"))
 {
  FGuid Id;
  if (!Fields(S, {TEXT("behavior_tree_path"), TEXT("kind"), TEXT("stable_node_id")}) || !Path(S.Elements[0])
   || S.Elements[2].Kind != EKind::String || !FGuid::ParseExact(S.Elements[2].StringValue, EGuidFormats::Digits, Id)
   || !Id.IsValid() || Id.ToString(EGuidFormats::Digits) != S.Elements[2].StringValue) return false;
 }
 else return false;
 for (const auto& Query : V.Elements[1].Elements) if (!QueryShape(Query)) return false;
 return true;
}
FAssetDocumentPrimitiveOutcome Inspect(UObject& Node, const FValue& V, bool bDefaults)
{
 const auto& BBValue = V.Elements[0];
 auto* BB = BBValue.Kind == EKind::Null ? nullptr : Cast<UBlackboardData>(FSoftObjectPath(BBValue.StringValue).ResolveObject());
 if ((BBValue.Kind != EKind::Null && (!BB || BB->GetClass() != UBlackboardData::StaticClass() || BB->GetPathName() != BBValue.StringValue))
  || !BlackboardChain(BB)) return Failure(TEXT("AIProvider.BehaviorTree.BindingInspectionUnavailable"), TEXT("The exact loaded Blackboard or its ancestry is unavailable."));
 FLocatorContext Context{Node, bDefaults}; FCursor Start;
 if (!Context.Object(Node, Start)) return Failure(TEXT("AIProvider.BehaviorTree.BindingInspectionUnavailable"), TEXT("Typed read-only node property rows are unavailable."));
 TArray<FValue> Evidence;
 bool bKeyMissing = false;
 for (int32 Index = 0; Index < V.Elements[1].Elements.Num(); ++Index)
 {
  const auto& Query = V.Elements[1].Elements[Index];
  FCursor Cursor; FValue Observation; FString Reason;
  if (!Context.Resolve(Start, *Field(Query, TEXT("locator")), Cursor))
   return Failure(TEXT("AIProvider.BehaviorTree.BindingLocatorRejected"), FString::Printf(TEXT("Binding query %d: %s"), Index, *Context.LastFailure));
  const FString& Kind = Field(Query, TEXT("kind"))->StringValue;
  const bool DecoratorSelector = Node.IsA(UBTDecorator_Blackboard::StaticClass()) && Field(Query, TEXT("locator"))->Elements.Num() == 1
   && Cursor.Property == FindFProperty<FProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("BlackboardKey"));
  const bool Valid = Kind == TEXT("selector") ? SelectorEvidence(Cursor, BB, Field(Query, TEXT("selected_key"))->StringValue, DecoratorSelector, Observation)
   : Kind == TEXT("value_or") ? ValueOrEvidence(Cursor, BB, Query, Observation, Reason) : SubtreeEvidence(Node, Cursor, BB, Query, Observation);
  if (!Valid) return Failure(TEXT("AIProvider.BehaviorTree.BindingInspectionUnavailable"),
   FString::Printf(TEXT("Binding query %d kind=%s leaf=%s: %s"), Index, *Kind, LeafType(Cursor) ? *LeafType(Cursor)->GetPathName()
    : Cursor.Property ? *Cursor.Property->GetName() : TEXT("unbound"), Reason.IsEmpty() ? TEXT("The exact typed evidence is unavailable.") : *Reason));
  const FValue* Exists = Kind == TEXT("selector") || Kind == TEXT("value_or") ? Field(Observation, TEXT("key_exists")) : nullptr;
  bKeyMissing |= Exists && Exists->Kind == EKind::Boolean && !Exists->BooleanValue && !Field(Query, TEXT("selected_key"))->StringValue.IsEmpty();
  Evidence.Add(MoveTemp(Observation));
 }
 FAssetDocumentPrimitiveOutcome Out;
 if (bKeyMissing && BB)
 {
  // The effective Blackboard's key names (its own, then its parents'): the close ones name the fix of an unknown key.
  TArray<FValue> Keys; TSet<FName> Seen;
  for (const UBlackboardData* Current = BB; Current; Current = Current->Parent)
   for (const FBlackboardEntry& Key : Current->Keys)
    if (!Key.EntryName.IsNone() && !Seen.Contains(Key.EntryName)) { Seen.Add(Key.EntryName); Keys.Add(StringValue(Key.EntryName.ToString())); }
  Out.Value = ObjectValue({TEXT("blackboard_keys"), TEXT("evidence")}, {ArrayValue(MoveTemp(Keys)), ArrayValue(MoveTemp(Evidence))});
 }
 else Out.Value = ObjectValue({TEXT("evidence")}, {ArrayValue(MoveTemp(Evidence))});
 if (!IsAssetDocumentAICanonicalRuntimeValue(Out.Value)) return Failure(TEXT("AIProvider.BehaviorTree.BindingInspectionUnavailable"), TEXT("Binding evidence exceeds the existing RuntimeValue envelope."));
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; return Out;
}
}

TOptional<FAssetDocumentPrimitiveOutcome> RejectAssetDocumentAIBehaviorTreeBindingInspectInvocation(
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentAtomicTransaction* Tx)
{
 using namespace AssetDocumentAIBehaviorTreeBindingInspect;
 if (I.PrimitiveId != TEXT("ai.behavior_tree.binding.inspect.v1") || I.Version != 1 || !IsInGameThread()
  || !GIsEditor || IsRunningCommandlet() || IsGarbageCollecting() || UE::IsSavingPackage(nullptr) || bInspecting
  || (I.Phase != EAssetDocumentRuntimeRequestPhase::Validate && I.Phase != EAssetDocumentRuntimeRequestPhase::Preflight
   && I.Phase != EAssetDocumentRuntimeRequestPhase::Verify))
  return Failure(TEXT("AIProvider.InvalidContext"), TEXT("Binding inspection requires its real read-only normal Editor phase."));
 if (!RequestShape(I.ValueArguments)) return Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("Binding inspection requires its exact finite query DTO."));
 if (Tx) return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("Binding inspection cannot enter an active transaction."));
 const bool Defaults = Field(I.ValueArguments.Elements[2], TEXT("kind"))->StringValue == TEXT("class_defaults");
 if ((Defaults && !I.HandleArguments.IsEmpty()) || (!Defaults && (I.HandleArguments.Num() != 1 || I.HandleArguments[0].Name != TEXT("asset"))))
  return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("Class defaults use no handle; live nodes require one read-only Asset lease."));
 return {};
}
FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeBindingInspect(
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Tx)
{
 using namespace AssetDocumentAIBehaviorTreeBindingInspect;
 if (auto Rejected = RejectAssetDocumentAIBehaviorTreeBindingInspectInvocation(I, Tx)) return MoveTemp(Rejected.GetValue());
 TGuardValue<bool> Guard(bInspecting, true);
 const auto& S = I.ValueArguments.Elements[2];
 if (Field(S, TEXT("kind"))->StringValue == TEXT("class_defaults"))
 {
  const FString& PathName = S.Elements[1].StringValue;
  auto* Class = FSoftClassPath(PathName).ResolveClass();
  if (!Class || Class->GetPathName() != PathName || !Class->GetDefaultObject(false))
   return Failure(TEXT("AIProvider.BehaviorTree.NodeClassUnavailable"), TEXT("The exact node class and its existing CDO must already be loaded."));
  if (!NodeFamily(*Class) || !Concrete(*Class)) return Failure(TEXT("AIProvider.BehaviorTree.NodeClassRejected"), TEXT("Class defaults require a concrete BehaviorTree node family."));
  return Inspect(*Class->GetDefaultObject(false), I.ValueArguments, true);
 }
 FAssetDocumentHandleValidationContext C;
 C.RequestId = I.RequestId; C.TransactionId = I.TransactionId; C.RuntimeRevision = I.RuntimeRevision;
 C.ProviderId = TEXT("ai"); C.Phase = I.Phase; C.RequiredKind = EAssetDocumentHandleKind::Asset; C.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
 auto Lease = Handles.AcquireVerifiedLease(I.HandleArguments[0].Handle, C);
 if (!Lease.Lease) return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The read-only AI Asset lease is invalid."));
 FAssetDocumentPrimitiveOutcome Out;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(*Lease.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  FAssetDocumentContractValidationResult Result;
  auto* Tree = Cast<UBehaviorTree>(&View.GetObject()); auto* Graph = Tree ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
  FString Before, After, Diagnostic;
  if (!Tree || Tree->GetClass() != UBehaviorTree::StaticClass() || View.GetCanonicalObjectPath() != S.Elements[0].StringValue
   || Tree->GetPathName() != View.GetCanonicalObjectPath() || static_cast<uint64>(Tree->GetUniqueID()) != View.GetTypeRevision()
   || !Graph || Graph->GetOuter() != Tree || !CaptureGraphIdentity(*Tree, Before, Diagnostic, EAuthoredGraphReadbackVersion::V2))
  { Out = Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The lease must own the exact live BehaviorTree authored graph.")); return Result; }
  auto* Node = UE::AssetDocument::AI::TypedPreimage::Selector::ResolveExactSelectorOwner(*Graph, S.Elements[2].StringValue);
  if (!Node || !NodeFamily(*Node->GetClass()) || !Node->IsIn(Tree))
  { Out = Failure(TEXT("AIProvider.BehaviorTree.BindingLocatorRejected"), TEXT("The stable node identity is not an exact authored BT instance.")); return Result; }
  Out = Inspect(*Node, I.ValueArguments, false);
  if (!CaptureGraphIdentity(*Tree, After, Diagnostic, EAuthoredGraphReadbackVersion::V2) || Before != After)
  { Out = Failure(TEXT("AIProvider.BehaviorTree.BindingInspectionUnavailable"), TEXT("Authored graph identity changed during read-only binding inspection.")); return Result; }
  Result.bValid = Out.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded; return Result;
 });
 return Identity.bValid || !Out.Diagnostics.IsEmpty() ? Out : Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The Asset lease could not enter its Editor identity."));
}
