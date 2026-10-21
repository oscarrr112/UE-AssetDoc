// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "BehaviorTree/AssetDocumentAIBehaviorTreeValueOrKeyInventory.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Tasks/BTTask_WaitBlackboardTime.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "IDetailTreeNode.h"
#include "IDetailCustomization.h"
#include "DetailLayoutBuilder.h"
#include "IPropertyRowGenerator.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Modules/ModuleManager.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StructOnScope.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include <type_traits>

namespace UE::AssetDocumentAI::BehaviorTreeValueOrKey
{
using namespace BehaviorTreeAuthoredReadback;
const TCHAR* FamilyName(EFamily Family)
{
 static const TCHAR* Names[] = {TEXT("Bool"),TEXT("Class"),TEXT("Enum"),TEXT("Float"),TEXT("Int32"),TEXT("Name"),TEXT("String"),TEXT("Object"),TEXT("Rotator"),TEXT("Struct"),TEXT("Vector")};
 return Names[static_cast<uint8>(Family)];
}
namespace
{
const TCHAR* Rejected = TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected");
bool EncodeHex(const FAssetDocumentRuntimeValueDto& Value, FString& Hex)
{
 TArray<uint8> Bytes;
 if (!EncodeAuthoredCanonicalValue(Value, Bytes) || Bytes.Num() > MAX_int32 / 2) return false;
 static const TCHAR Digits[] = TEXT("0123456789abcdef");
 Hex.Reset(Bytes.Num() * 2);
 for (uint8 Byte : Bytes) { Hex.AppendChar(Digits[Byte >> 4]); Hex.AppendChar(Digits[Byte & 15]); }
 return true;
}
bool Digest(const FAssetDocumentRuntimeValueDto& Value, FString& Out)
{
 FString Hex;
 if (!EncodeHex(Value, Hex)) return false;
 Out = BehaviorTreeDerivedCompletion::Private::HashEvidence(Hex); return true;
}
FAssetDocumentRuntimeValueDto Schema(const FProperty& P)
{
 const FStructProperty* S = CastField<FStructProperty>(&P);
 return ArrayValue({StringValue(P.GetOwnerStruct() ? P.GetOwnerStruct()->GetPathName() : FString()),
  StringValue(P.GetName()), StringValue(P.GetClass()->GetName()),
  StringValue(S && S->Struct ? S->Struct->GetPathName() : FString())});
}
bool Family(const UScriptStruct* S, EFamily& Out)
{
#define BT_FAMILY(N) if (S == FValueOrBBKey_##N::StaticStruct()) { Out = EFamily::N; return true; }
 BT_FAMILY(Bool) BT_FAMILY(Class) BT_FAMILY(Enum) BT_FAMILY(Float) BT_FAMILY(Int32)
 BT_FAMILY(Name) BT_FAMILY(String) BT_FAMILY(Object) BT_FAMILY(Rotator) BT_FAMILY(Struct) BT_FAMILY(Vector)
#undef BT_FAMILY
 return false;
}
// This closed cast exists only at the immediate typed boundary. No raw memory is
// returned, persisted, indexed or used for field access.
bool WithReference(IPropertyHandle& Handle, EFamily Family,
 TFunctionRef<bool(FExactLeafReference&)> Callback)
{
 void* Address = nullptr;
 if (Handle.GetValueData(Address) != FPropertyAccess::Success || !Address) return false;
 switch (Family)
 {
#define BT_REFERENCE(N) case EFamily::N: { FExactLeafReference Ref(TInPlaceType<FValueOrBBKey_##N*>(), static_cast<FValueOrBBKey_##N*>(Address)); return Callback(Ref); }
 BT_REFERENCE(Bool) BT_REFERENCE(Class) BT_REFERENCE(Enum) BT_REFERENCE(Float) BT_REFERENCE(Int32)
 BT_REFERENCE(Name) BT_REFERENCE(String) BT_REFERENCE(Object) BT_REFERENCE(Rotator) BT_REFERENCE(Struct) BT_REFERENCE(Vector)
#undef BT_REFERENCE
 }
 return false;
}
struct FBoundLeaf { FLeaf Leaf; TSharedRef<IPropertyHandle> Handle; };
struct FScan
{
 FInventory Inventory;
 TArray<TSharedPtr<IPropertyRowGenerator>> Rows;
 TMap<const UObject*,TSharedRef<IPropertyHandle>> StockWaitSelectors;
 TArray<FBoundLeaf> Bound;
 TSet<const UObject*> Instances;
 TSet<const UEdGraphNode*> Nodes;
 FPropertyReadContext Budget;
 FAssetDocumentRuntimeValueDto MappingProjection, BlackboardChain;
 UBlackboardData* Blackboard = nullptr;
 TMap<FString, const UEnum*> EnumNodes;
 FString Diagnostic = Rejected;
};
bool Read(IPropertyHandle& Handle, FScan& Scan, FAssetDocumentRuntimeValueDto& Value)
{
 return ReadAuthoredProperty(Handle, Scan.Budget, 1, Value);
}
bool HasLeafType(const FProperty* P, int32 Depth = 0)
{
 if (!P || Depth > 64) return true;
 if (const FStructProperty* S = CastField<FStructProperty>(P))
 {
  if (S->Struct->IsChildOf(FValueOrBlackboardKeyBase::StaticStruct())) return true;
  if (S->Struct == FInstancedStruct::StaticStruct()) return true;
  for (TFieldIterator<FProperty> It(S->Struct); It; ++It) if (HasLeafType(*It, Depth + 1)) return true;
 }
 if (const FArrayProperty* A = CastField<FArrayProperty>(P)) return HasLeafType(A->Inner, Depth + 1);
 if (const FSetProperty* A = CastField<FSetProperty>(P)) return HasLeafType(A->ElementProp, Depth + 1);
 if (const FMapProperty* A = CastField<FMapProperty>(P)) return HasLeafType(A->KeyProp, Depth + 1) || HasLeafType(A->ValueProp, Depth + 1);
 return false;
}
// InstancedStruct's declared type alone says nothing about its current leaves.
// Inspect its public generated children, without following UObject references.
bool HasActualLeaf(IPropertyHandle& Handle, int32 Depth = 0)
{
 if (Depth > 64) return true;
 const FProperty* P = Handle.GetProperty();
 if (const FStructProperty* S = CastField<FStructProperty>(P))
  if (S->Struct->IsChildOf(FValueOrBlackboardKeyBase::StaticStruct())) return true;
 if (P && CastField<FObjectPropertyBase>(P)) return false;
 uint32 Count = 0;
 if (Handle.GetNumChildren(Count) != FPropertyAccess::Success || Count > 4096) return true;
 for (uint32 Index = 0; Index < Count; ++Index)
 {
  const auto Child = Handle.GetChildHandle(Index);
  if (!Child || HasActualLeaf(*Child, Depth + 1)) return true;
 }
 return false;
}
bool CheckLeafSchema(IPropertyHandle& Handle, EFamily Family, FAssetDocumentRuntimeValueDto& OutSchema)
{
 const FStructProperty* P = CastField<FStructProperty>(Handle.GetProperty());
 if (!P || !P->Struct) return false;
 TArray<FName> Fields = {TEXT("DefaultValue"),TEXT("Key")};
 if (Family == EFamily::Class || Family == EFamily::Object) Fields.Add(TEXT("BaseClass"));
 if (Family == EFamily::Enum) { Fields.Add(TEXT("EnumType")); Fields.Add(TEXT("NativeEnumTypeName")); }
 if (Family == EFamily::Struct) Fields.Add(TEXT("bCanEditDefaultValueType"));
 TArray<FAssetDocumentRuntimeValueDto> Values;
 for (const FName Name : Fields)
 {
  const TSharedPtr<IPropertyHandle> H = Handle.GetChildHandle(Name, false);
  if (!H || !IsAuthoredProperty(*H)) return false;
  const FProperty* Field = H->GetProperty();
  const UStruct* Owner = Name == TEXT("Key") ? FValueOrBlackboardKeyBase::StaticStruct() : P->Struct.Get();
  const FFieldClass* Expected = nullptr;
  if (Name == TEXT("Key")) Expected = FNameProperty::StaticClass();
  else if (Name == TEXT("BaseClass")) Expected = FClassProperty::StaticClass();
  else if (Name == TEXT("EnumType")) Expected = FObjectProperty::StaticClass();
  else if (Name == TEXT("NativeEnumTypeName")) Expected = FStrProperty::StaticClass();
  else if (Name == TEXT("bCanEditDefaultValueType")) Expected = FBoolProperty::StaticClass();
  else switch (Family)
  {
   case EFamily::Bool: Expected = FBoolProperty::StaticClass(); break;
   case EFamily::Class: Expected = FClassProperty::StaticClass(); break;
   case EFamily::Enum: Expected = FByteProperty::StaticClass(); break;
   case EFamily::Float: Expected = FFloatProperty::StaticClass(); break;
   case EFamily::Int32: Expected = FIntProperty::StaticClass(); break;
   case EFamily::Name: Expected = FNameProperty::StaticClass(); break;
   case EFamily::String: Expected = FStrProperty::StaticClass(); break;
   case EFamily::Object: Expected = FObjectProperty::StaticClass(); break;
   default: Expected = FStructProperty::StaticClass(); break;
  }
  if (Field->GetOwnerStruct() != Owner || Field->GetFName() != Name || Field->GetClass() != Expected) return false;
  if (const FStructProperty* S = CastField<FStructProperty>(Field))
  {
   const UScriptStruct* Type = Family == EFamily::Struct ? FInstancedStruct::StaticStruct()
    : Family == EFamily::Vector ? TBaseStructure<FVector>::Get() : TBaseStructure<FRotator>::Get();
   if (S->Struct != Type) return false;
  }
  Values.Add(Schema(*Field));
 }
 OutSchema = ArrayValue(MoveTemp(Values)); return true;
}
bool Walk(TSharedRef<IPropertyHandle> Handle, TArray<FAssetDocumentRuntimeValueDto> Path,
 FScan& Scan, int32 Depth, bool Forbidden = false)
{
 if (Depth > 64 || --Scan.Budget.RemainingNodes < 0) return false;
 const FProperty* P = Handle->GetProperty();
 if (!P || !Handle->IsValidHandle()) return false;
 Path.Add(Schema(*P));
 const auto Array = Handle->AsArray(); const auto Map = Handle->AsMap(); const auto Set = Handle->AsSet();
 if (Array || Map || Set)
 {
  uint32 Count = 0;
  if ((Array ? Array->GetNumElements(Count) : Map ? Map->GetNumElements(Count) : Set->GetNumElements(Count)) != FPropertyAccess::Success || Count > 4096) return false;
  TMap<FString, FString> Identities;
  for (uint32 Index = 0; Index < Count; ++Index)
  {
   auto Element = Array ? Array->GetElement(Index) : Map ? Map->GetElement(Index) : Set->GetElement(Index);
   TArray<FAssetDocumentRuntimeValueDto> ChildPath = Path;
   FAssetDocumentRuntimeValueDto Value;
   auto IdentityHandle = Map ? Element->GetKeyHandle() : TSharedPtr<IPropertyHandle>(Element);
   if (!IdentityHandle || !Read(*IdentityHandle, Scan, Value)) return false;
   if (Map && HasActualLeaf(*IdentityHandle)) return false;
   FString Encoding;
   if (!EncodeHex(ArrayValue({StringValue(Map ? TEXT("assetdocument.ai.bt.value_or.map_key.v1") : TEXT("assetdocument.ai.bt.value_or.element.v1")),
    Value, Schema(*IdentityHandle->GetProperty())}), Encoding)) return false;
   const FString Identity = BehaviorTreeDerivedCompletion::Private::HashEvidence(Encoding);
   if (const FString* Previous = Identities.Find(Identity))
   {
    // Reject a collision even outside a leaf-bearing element. Equal authored
    // siblings are ambiguous only when the element actually carries a leaf.
    if (*Previous != Encoding || HasActualLeaf(*Element)) return false;
   }
   Identities.Add(Identity, MoveTemp(Encoding)); ChildPath.Add(StringValue(Identity));
   if (!Walk(Element, MoveTemp(ChildPath), Scan, Depth + 1, Forbidden || Set.IsValid())) return false;
  }
  return true;
 }
 const FStructProperty* Struct = CastField<FStructProperty>(P);
 if (!Struct) return true; // Object references are terminal, never traversed.
 EFamily Exact;
 if (Family(Struct->Struct, Exact))
 {
  if (Forbidden) return false;
  FLeaf Leaf; Leaf.Family = Exact;
  if (!CheckLeafSchema(*Handle, Exact, Leaf.SchemaValue)) { Scan.Diagnostic = TEXT("AIProvider.AuthorityUnavailable"); return false; }
  Leaf.LocatorValue = ArrayValue(Path);
  if (!Read(*Handle, Scan, Leaf.Authored) || !EncodeHex(Leaf.LocatorValue, Leaf.Locator)
   || !EncodeHex(Leaf.SchemaValue, Leaf.Schema)) return false;
  const TSharedPtr<IPropertyHandle> Key = Handle->GetChildHandle(TEXT("Key"), false);
  if (!Key || Key->GetValue(Leaf.Key) != FPropertyAccess::Success) return false;
  Scan.Bound.Add({MoveTemp(Leaf), Handle}); return true;
 }
 if (Struct->Struct->IsChildOf(FValueOrBlackboardKeyBase::StaticStruct())) return false;
 uint32 Count = 0;
 if (Handle->GetNumChildren(Count) != FPropertyAccess::Success || Count > 4096) return false;
 if (Struct->Struct == FInstancedStruct::StaticStruct())
 {
  TSharedPtr<IPropertyHandle> Wrapper;
  TSharedPtr<FStructOnScope> Scope;
  for (uint32 Index = 0; Index < Count; ++Index)
  {
   auto Child = Handle->GetChildHandle(Index); if (!Child) return false;
   if (!Child->AsStruct() && !CastField<FStructProperty>(Child->GetProperty())) continue;
   TArray<TSharedPtr<FStructOnScope>> Scopes; Child->GetOuterStructs(Scopes);
   if (Scopes.Num() != 1 || Wrapper) return false;
   Wrapper = Child; Scope = Scopes[0];
  }
  if (!Wrapper) return false;
  if (!Scope) return true;
  if (!Scope->GetStruct()) return false;
  Path.Add(StringValue(Scope->GetStruct()->GetPathName()));
  const FStructProperty* WrapperProperty = CastField<FStructProperty>(Wrapper->GetProperty());
  if (WrapperProperty)
  {
   if (WrapperProperty->Struct != Scope->GetStruct()) return false;
   return Walk(Wrapper.ToSharedRef(), MoveTemp(Path), Scan, Depth + 1, Forbidden);
  }
  Handle = Wrapper.ToSharedRef();
  if (Handle->GetNumChildren(Count) != FPropertyAccess::Success || Count > 4096) return false;
 }
 for (uint32 Index = 0; Index < Count; ++Index)
 {
  auto Child = Handle->GetChildHandle(Index); if (!Child || !Child->GetProperty()) return false;
  if (!IsAuthoredProperty(*Child)) { if (HasActualLeaf(*Child)) return false; continue; }
  if (!Walk(Child.ToSharedRef(), Path, Scan, Depth + 1, Forbidden)) return false;
 }
 return true;
}
bool Gather(const TArray<TSharedRef<IDetailTreeNode>>& Nodes, UObject& Owner,
 TMap<FString,TSharedRef<IPropertyHandle>>& Roots, TSet<FString>& SafeExcluded, FScan& Scan, int32 Depth = 0)
{
 if (Depth > 64) return false;
 for (const auto& Node : Nodes)
 {
  if (--Scan.Budget.RemainingNodes < 0) return false;
  auto Handle = Node->CreatePropertyHandle(); TSharedPtr<IPropertyHandle> Root;
  for (int32 D = 0; Handle && D <= 64; ++D)
  {
   const FProperty* P = Handle->GetProperty();
   if (P && P->GetOwnerProperty() == P && Cast<UClass>(P->GetOwnerStruct())) Root = Handle;
   Handle = Handle->GetParentHandle(); if (D == 64 && Handle) return false;
  }
  if (Root)
  {
   TArray<UObject*> Outers; Root->GetOuterObjects(Outers);
   if (Outers.Num() != 1 || Outers[0] != &Owner) return false;
   if (IsAuthoredProperty(*Root)) Roots.Add(Root->GetProperty()->GetName(), Root.ToSharedRef());
   else if (HasActualLeaf(*Root)) return false;
   else SafeExcluded.Add(Root->GetProperty()->GetName());
  }
  TArray<TSharedRef<IDetailTreeNode>> Children; Node->GetChildren(Children, true);
  if (!Gather(Children, Owner, Roots, SafeExcluded, Scan, Depth + 1)) return false;
 }
 return true;
}
// This stock deprecated task hides the two inherited Wait fields, but its
// InitializeFromAsset still uses WaitTime. Generate only their typed data nodes;
// no row widget may run the stock ValueOr header's editing ValidateData.
struct FHiddenWaitRows
{
 TWeakObjectPtr<UObject> Owner;
 TMap<FString,TSharedRef<IPropertyHandle>> Handles;
 bool Valid = false;
};
const FStructProperty* HiddenWaitProperty(UObject& Owner, FName Name)
{
 if (Owner.GetClass() != UBTTask_WaitBlackboardTime::StaticClass()
  || (Name != GET_MEMBER_NAME_CHECKED(UBTTask_Wait, WaitTime)
   && Name != GET_MEMBER_NAME_CHECKED(UBTTask_Wait, RandomDeviation))) return nullptr;
 const FStructProperty* Property = FindFProperty<FStructProperty>(Owner.GetClass(), Name);
 return Property && Property == FindFProperty<FStructProperty>(UBTTask_Wait::StaticClass(), Name)
  && Property->GetOwnerStruct() == UBTTask_Wait::StaticClass() && Property->GetOwnerProperty() == Property
  && Property->ArrayDim == 1 && Property->Struct == FValueOrBBKey_Float::StaticStruct()
  && Property->GetMetaData(TEXT("Category")) == TEXT("Wait") && Property->HasAnyPropertyFlags(CPF_Edit)
  && !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst) ? Property : nullptr;
}
class FHiddenWaitCustomization final : public IDetailCustomization
{
public:
 explicit FHiddenWaitCustomization(TSharedRef<FHiddenWaitRows> InRows) : HiddenRows(MoveTemp(InRows)) {}
 void CustomizeDetails(IDetailLayoutBuilder& Builder) override
 {
  HiddenRows->Valid = false;
  HiddenRows->Handles.Reset();
  UObject* Owner = HiddenRows->Owner.Get();
  TArray<TWeakObjectPtr<UObject>> Objects;
  Builder.GetObjectsBeingCustomized(Objects);
  if (!Owner || Owner->GetClass() != UBTTask_WaitBlackboardTime::StaticClass()
   || Objects.Num() != 1 || Objects[0].Get() != Owner) return;
  const TArray<UObject*> Owners = {Owner};
  for (const FName Name : {GET_MEMBER_NAME_CHECKED(UBTTask_Wait, WaitTime),
   GET_MEMBER_NAME_CHECKED(UBTTask_Wait, RandomDeviation)})
  {
   const FStructProperty* Property = HiddenWaitProperty(*Owner, Name);
   if (!Property) return;
   const TSharedPtr<IPropertyHandle> Handle = Builder.AddObjectPropertyData(Owners, Name);
   if (!Handle || Handle->GetProperty() != Property || !IsAuthoredProperty(*Handle)) return;
   TArray<UObject*> Outers; Handle->GetOuterObjects(Outers);
   if (Outers.Num() != 1 || Outers[0] != Owner) return;
   HiddenRows->Handles.Add(Property->GetName(), Handle.ToSharedRef());
  }
  HiddenRows->Valid = HiddenRows->Handles.Num() == 2;
 }
private:
 TSharedRef<FHiddenWaitRows> HiddenRows;
};
bool Instance(UObject& Object, const FString& NodeIdentity, FScan& Scan)
{
 if (Scan.Instances.Contains(&Object)) return false;
 Scan.Instances.Add(&Object);
 auto* Module = FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
 if (!Module) { Scan.Diagnostic = TEXT("AIProvider.AuthorityUnavailable"); return false; }
 FPropertyRowGeneratorArgs Args; Args.bAllowEditingClassDefaultObjects = true;
 auto Rows = Module->CreatePropertyRowGenerator(Args);
 ConfigureReadOnlyValueOrRows(*Rows);
 TSharedPtr<FHiddenWaitRows> HiddenWait;
 if (Object.GetClass() == UBTTask_WaitBlackboardTime::StaticClass())
 {
  HiddenWait = MakeShared<FHiddenWaitRows>(); HiddenWait->Owner = &Object;
  Rows->RegisterInstancedCustomPropertyLayout(Object.GetClass(),
   FOnGetDetailCustomizationInstance::CreateLambda([State = HiddenWait.ToSharedRef()]() -> TSharedRef<IDetailCustomization>
   { return MakeShared<FHiddenWaitCustomization>(State); }));
 }
 Rows->SetObjects({&Object}); Scan.Rows.Add(Rows);
 TMap<FString,TSharedRef<IPropertyHandle>> Roots;
 TSet<FString> SafeExcluded;
 if (!Gather(Rows->GetRootTreeNodes(), Object, Roots, SafeExcluded, Scan)) return false;
 if (HiddenWait)
 {
  if (!HiddenWait->Valid) return false;
  for (const auto& Pair : HiddenWait->Handles)
  {
   if (SafeExcluded.Contains(Pair.Key)) return false;
   if (const auto* Existing = Roots.Find(Pair.Key))
    if ((*Existing)->GetProperty() != Pair.Value->GetProperty()) return false;
   Roots.Add(Pair.Key, Pair.Value);
  }
  if (const auto* Selector = Roots.Find(TEXT("BlackboardKey"))) Scan.StockWaitSelectors.Add(&Object, *Selector);
 }
 // Ensure detail customization cannot silently hide an eligible ValueOr field.
 for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
 {
  if (!SafeExcluded.Contains(It->GetName()) && HasLeafType(*It) && (!It->HasAnyPropertyFlags(CPF_Edit)
   || It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst)
   || !Roots.Contains(It->GetName()))) return false;
 }
 for (const auto& Pair : Roots)
  if (!Walk(Pair.Value, {StringValue(NodeIdentity),StringValue(Object.GetClass()->GetPathName())}, Scan, 1)) return false;
 if (const auto* Decorator = Cast<UBTDecorator_Blackboard>(&Object))
 {
  const FBlackboard::FKey ID = Scan.Blackboard
   ? Scan.Blackboard->GetKeyID(Decorator->GetSelectedBlackboardKey()) : FBlackboard::InvalidKey;
  const FBlackboardEntry* Entry = Scan.Blackboard && ID != FBlackboard::InvalidKey ? Scan.Blackboard->GetKey(ID) : nullptr;
  const UEnum* Enum = nullptr;
  if (Entry && Entry->KeyType)
  {
   if (Entry->KeyType->GetClass() == UBlackboardKeyType_Enum::StaticClass())
    Enum = CastChecked<UBlackboardKeyType_Enum>(Entry->KeyType)->EnumType.Get();
   else if (Entry->KeyType->GetClass() == UBlackboardKeyType_NativeEnum::StaticClass())
    Enum = CastChecked<UBlackboardKeyType_NativeEnum>(Entry->KeyType)->EnumType.Get();
  }
  if (Enum)
  {
   const auto* Int = FindFProperty<FIntProperty>(Object.GetClass(), TEXT("IntValue"));
   const auto* String = FindFProperty<FStrProperty>(Object.GetClass(), TEXT("StringValue"));
   if (!Int || !String || Int->GetOwnerStruct() != UBTDecorator_Blackboard::StaticClass()
    || String->GetOwnerStruct() != UBTDecorator_Blackboard::StaticClass()) return false;
   // Every constructed node identity ends in its validated persistent NodeGuid.
   const FString Guid = NodeIdentity.Right(32);
   if (Scan.EnumNodes.Contains(Guid)) return false;
   Scan.EnumNodes.Add(Guid, Enum);
  }
 }
 return true;
}
bool Node(UBehaviorTreeGraphNode& N, const FString& Identity, FScan& Scan)
{
 if (N.bInjectedNode) return true;
 if (!N.NodeGuid.IsValid() || Scan.Nodes.Contains(&N)) return false;
 Scan.Nodes.Add(&N);
 const FString Id = Identity + TEXT("/") + N.NodeGuid.ToString(EGuidFormats::Digits);
 if (N.NodeInstance && !Instance(*N.NodeInstance, Id, Scan)) return false;
 if (auto* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(&N))
 {
  if (Composite->BoundGraph)
  {
   if (!Composite->BoundGraph->GraphGuid.IsValid() || Composite->BoundGraph->GetOuter() != Composite) return false;
   for (UEdGraphNode* Bound : Composite->BoundGraph->Nodes)
   {
    if (!Bound || !Bound->NodeGuid.IsValid()) return false;
    if (auto* Test = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Bound))
    {
     if (!Test->NodeInstance || !Instance(*Test->NodeInstance, Id + TEXT("/bound/")
      + Composite->BoundGraph->GraphGuid.ToString(EGuidFormats::Digits) + TEXT("/")
      + Bound->NodeGuid.ToString(EGuidFormats::Digits), Scan)) return false;
    }
   }
  }
 }
 for (auto& D : N.Decorators) if (!D || !Node(*D, Id + TEXT("/decorator"), Scan)) return false;
 for (auto& S : N.Services) if (!S || !Node(*S, Id + TEXT("/service"), Scan)) return false;
 return true;
}
template<typename T>
bool CheckPublicLiteral(T& Leaf, IPropertyHandle& Handle)
{
 if constexpr (std::is_same_v<T,FValueOrBBKey_Bool> || std::is_same_v<T,FValueOrBBKey_Float>
  || std::is_same_v<T,FValueOrBBKey_Int32> || std::is_same_v<T,FValueOrBBKey_Name>
  || std::is_same_v<T,FValueOrBBKey_String> || std::is_same_v<T,FValueOrBBKey_Rotator>
  || std::is_same_v<T,FValueOrBBKey_Vector>)
 {
  const auto Public = Leaf.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
  // FString property export appends to its destination; start every observation empty.
  std::decay_t<decltype(Public)> Projected{};
  const auto Default = Handle.GetChildHandle(TEXT("DefaultValue"), false);
  return Default && Default->GetValue(Projected) == FPropertyAccess::Success && Public == Projected;
 }
 else
 {
  return true; // Policy-bearing families use the complete D2 authored projection.
 }
}
bool CaptureBlackboardChain(UBehaviorTree& Tree, FScan& Scan)
{
 UBlackboardData* BB = Tree.BlackboardAsset;
 TArray<FAssetDocumentRuntimeValueDto> Chain;
 TSet<const UBlackboardData*> Seen;
 TSet<FName> Names;
 for (const UBlackboardData* Current = BB; Current; Current = Current->Parent)
 {
  const FSoftObjectPath Path(Current);
  if (!IsValid(Current) || Seen.Contains(Current) || Seen.Num() >= 64
   || Current->GetOuter() != Current->GetPackage() || !Path.IsValid()
   || !Path.GetSubPathString().IsEmpty() || Path.ToString() != Current->GetPathName()) return false;
  Seen.Add(Current);
  TArray<FAssetDocumentRuntimeValueDto> Keys;
  for (const FBlackboardEntry& Entry : Current->Keys)
  {
   if (!Entry.KeyType || Entry.EntryName.IsNone() || Names.Contains(Entry.EntryName)) return false;
   Names.Add(Entry.EntryName);
   if (--Scan.Budget.RemainingNodes < 0) return false;
   FAssetDocumentRuntimeValueDto Properties;
   if (!BuildAuthoredProperties(*Entry.KeyType, Properties)) return false;
   Keys.Add(ArrayValue({StringValue(Entry.EntryName.ToString()),StringValue(Entry.KeyType->GetClass()->GetPathName()),MoveTemp(Properties)}));
  }
  Chain.Add(ArrayValue({StringValue(Current->GetPathName()),ArrayValue(MoveTemp(Keys))}));
 }
 Scan.BlackboardChain = ArrayValue(MoveTemp(Chain));
 return true;
}
bool ObserveMapping(UBehaviorTree& Tree, FScan& Scan)
{
 UBlackboardData* BB = Tree.BlackboardAsset;
 TArray<FAssetDocumentRuntimeValueDto> Mappings;
 for (FBoundLeaf& Bound : Scan.Bound)
 {
  const FBlackboard::FKey ID = BB && !Bound.Leaf.Key.IsNone() ? BB->GetKeyID(Bound.Leaf.Key) : FBlackboard::InvalidKey;
  const FBlackboardEntry* Entry = BB && ID != FBlackboard::InvalidKey ? BB->GetKey(ID) : nullptr;
  if (!WithReference(*Bound.Handle, Bound.Leaf.Family, [&](FExactLeafReference& Ref)
  {
   return Visit([&](auto* Typed)
   {
    const bool LiteralMatches = Typed && CheckPublicLiteral(*Typed, *Bound.Handle);
    const bool KeyMatches = Typed && Typed->GetKey() == Bound.Leaf.Key;
    const bool TypeMatches = Typed && (!Entry || (Entry->KeyType && Typed->IsCompatibleType(Entry->KeyType)));
    if (!LiteralMatches || !KeyMatches || !TypeMatches)
     UE_LOG(LogTemp, Display, TEXT("ValueOr mapping rejected: family=%s key=%s literal=%d keyIdentity=%d type=%d"),
      FamilyName(Bound.Leaf.Family), *Bound.Leaf.Key.ToString(), LiteralMatches, KeyMatches, TypeMatches);
    return LiteralMatches && KeyMatches && TypeMatches;
   }, Ref);
  })) { Scan.Diagnostic = TEXT("AIProvider.BehaviorTree.ValueOrKeyTypeMismatch"); return false; }
  FAssetDocumentRuntimeValueDto KeySchema = NullValue();
  if (Entry && (!Entry->KeyType || !BuildAuthoredProperties(*Entry->KeyType, KeySchema))) return false;
  Mappings.Add(ArrayValue({Bound.Leaf.LocatorValue,StringValue(FamilyName(Bound.Leaf.Family)),
   StringValue(Bound.Leaf.Key.ToString()),IntegerValue(ID == FBlackboard::InvalidKey ? -1 : static_cast<int32>(ID)),
   StringValue(Entry && Entry->KeyType ? Entry->KeyType->GetClass()->GetPathName() : FString()),MoveTemp(KeySchema)}));
 }
 Scan.Inventory.BlackboardPath = BB ? BB->GetPathName() : FString();
 Scan.MappingProjection = ArrayValue({StringValue(TEXT("assetdocument.ai.bt.value_or.mapping.v1")),StringValue(Scan.Inventory.BlackboardPath),
  Scan.BlackboardChain,ArrayValue(MoveTemp(Mappings))});
 return Digest(Scan.MappingProjection, Scan.Inventory.MappingDigest);
}
FAssetDocumentRuntimeValueDto* Field(FAssetDocumentRuntimeValueDto& Value, const TCHAR* Key)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::Object || Value.ObjectKeys.Num() != Value.Elements.Num()) return nullptr;
 const int32 Index = Value.ObjectKeys.IndexOfByKey(Key);
 return Index == INDEX_NONE ? nullptr : &Value.Elements[Index];
}
// Follow only D2's known graph/node envelopes; never search arbitrary authored
// property DTOs for similarly named keys. The wire projection is left untouched.
bool NormalizeNode(FAssetDocumentRuntimeValueDto& NodeValue, const FScan& Scan, TSet<FString>& Seen)
{
 if (NodeValue.Kind == EAssetDocumentRuntimeValueKind::Null) return true;
 auto* Guid = Field(NodeValue, TEXT("stable_node_id"));
 auto* Properties = Field(NodeValue, TEXT("authored_properties"));
 if (!Guid || Guid->Kind != EAssetDocumentRuntimeValueKind::String || !Properties) return false;
 if (const UEnum* const* Enum = Scan.EnumNodes.Find(Guid->StringValue))
 {
  if (Seen.Contains(Guid->StringValue)) return false;
  Seen.Add(Guid->StringValue);
  auto* Int = Field(*Properties, TEXT("IntValue"));
  auto* String = Field(*Properties, TEXT("StringValue"));
  if (!Int || Int->Kind != EAssetDocumentRuntimeValueKind::Integer
   || !String || String->Kind != EAssetDocumentRuntimeValueKind::String) return false;
  const int64 Value = (*Enum)->GetValueByNameString(String->StringValue);
  if (Value < MIN_int32 || Value > MAX_int32) return false;
  *Int = IntegerValue(Value);
 }
 for (const TCHAR* Collection : {TEXT("children"),TEXT("decorators"),TEXT("services")})
  if (auto* Values = Field(NodeValue, Collection))
  {
   if (Values->Kind != EAssetDocumentRuntimeValueKind::Array) return false;
   for (auto& Child : Values->Elements) if (!NormalizeNode(Child, Scan, Seen)) return false;
  }
 if (auto* Bound = Field(NodeValue, TEXT("bound_graph")))
  if (Bound->Kind != EAssetDocumentRuntimeValueKind::Null)
  {
   auto* Nodes = Field(*Bound, TEXT("nodes"));
   if (!Nodes || Nodes->Kind != EAssetDocumentRuntimeValueKind::Array) return false;
   for (auto& Child : Nodes->Elements) if (!NormalizeNode(Child, Scan, Seen)) return false;
  }
 return true;
}
bool ScanTree(UBehaviorTree& Tree, FScan& Scan)
{
 if (!IsInGameThread() || IsRunningCommandlet() || !GIsEditor || IsGarbageCollecting()
  || Tree.GetClass() != UBehaviorTree::StaticClass()) { Scan.Diagnostic = TEXT("AIProvider.AuthorityUnavailable"); return false; }
 FString Before, After, Diagnostic;
 const auto Version = Scan.Inventory.ProjectionVersion;
 if (!CaptureGraphIdentity(Tree, Before, Diagnostic, Version)) return false;
 auto* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph);
 if (!Graph || Graph->GetOuter() != &Tree || !Graph->GraphGuid.IsValid()) return false;
 FAssetDocumentRuntimeValueDto Projection; bool Overflow = false;
 if (!BuildAuthoredGraphProjection(Tree, Projection, Overflow, Version)) return false;
 Scan.Inventory.TreePath = Tree.GetPathName(); Scan.Inventory.PackagePath = Tree.GetPackage()->GetName();
 Scan.Inventory.GraphIdentity = Graph->GraphGuid.ToString(EGuidFormats::Digits);
 if (!HashAuthoredGraphValue(Scan.Inventory.TreePath, Projection, Scan.Inventory.AuthoredDigest, Version)) return false;
 Scan.Blackboard = Tree.BlackboardAsset;
 if (!CaptureBlackboardChain(Tree, Scan)) return false;
 for (UEdGraphNode* GraphNode : Graph->Nodes)
 {
  if (auto* Main = Cast<UBehaviorTreeGraphNode>(GraphNode))
   if (!Main->IsSubNode() && !Node(*Main, Scan.Inventory.GraphIdentity + TEXT("/main"), Scan)) return false;
 }
 Scan.Bound.Sort([](const FBoundLeaf& A, const FBoundLeaf& B) { return A.Leaf.Locator < B.Leaf.Locator; });
 TArray<FAssetDocumentRuntimeValueDto> TypedRows;
 for (int32 Index = 0; Index < Scan.Bound.Num(); ++Index)
 {
  const FLeaf& Leaf = Scan.Bound[Index].Leaf;
  if (Index && Scan.Bound[Index - 1].Leaf.Locator == Leaf.Locator) return false;
  ++Scan.Inventory.FamilyCounts[static_cast<uint8>(Leaf.Family)];
  Scan.Inventory.Leaves.Add(Leaf);
  TypedRows.Add(ArrayValue({Leaf.LocatorValue,StringValue(FamilyName(Leaf.Family)),Leaf.SchemaValue,Leaf.Authored}));
 }
 const auto Typed = ArrayValue({StringValue(TEXT("assetdocument.ai.bt.value_or.typed_inventory.v1")),
  StringValue(Scan.Inventory.TreePath),StringValue(Scan.Inventory.GraphIdentity),ArrayValue(MoveTemp(TypedRows))});
 // Count the complete projection and metadata together, not independent 4096-node envelopes.
 if (!IsAssetDocumentAICanonicalRuntimeValue(ArrayValue({Projection,Typed}))) return false;
 if (!Digest(Typed, Scan.Inventory.TypedDigest) || !ObserveMapping(Tree, Scan)) return false;
 if (!IsAssetDocumentAICanonicalRuntimeValue(ArrayValue({Projection,Typed,Scan.MappingProjection}))) return false;
 FAssetDocumentRuntimeValueDto Semantic = Projection;
 auto* Root = Field(Semantic, TEXT("root"));
 TSet<FString> Normalized;
 if (!Root || !NormalizeNode(*Root, Scan, Normalized) || Normalized.Num() != Scan.EnumNodes.Num()
  || !HashAuthoredGraphValue(Scan.Inventory.TreePath, Semantic, Scan.Inventory.SemanticAuthoredDigest, Version)) return false;
 Scan.Inventory.AuthoredProjection = MoveTemp(Projection);
 Scan.Inventory.TypedProjection = Typed;
 Scan.Inventory.MappingProjection = MoveTemp(Scan.MappingProjection);
 return CaptureGraphIdentity(Tree, After, Diagnostic, Version) && Before == After;
}
bool EqualEvidence(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
 TArray<uint8> Left, Right;
 return EncodeAuthoredCanonicalValue(A, Left) && EncodeAuthoredCanonicalValue(B, Right) && Left == Right;
}
bool EqualInventory(const FInventory& A, const FInventory& B)
{
 return A.ProjectionVersion == B.ProjectionVersion && A.TreePath == B.TreePath && A.PackagePath == B.PackagePath
  && A.GraphIdentity == B.GraphIdentity && A.BlackboardPath == B.BlackboardPath && A.AuthoredDigest == B.AuthoredDigest
  && A.SemanticAuthoredDigest == B.SemanticAuthoredDigest && A.TypedDigest == B.TypedDigest && A.MappingDigest == B.MappingDigest
  && A.FamilyCounts == B.FamilyCounts && A.Leaves.Num() == B.Leaves.Num()
  && EqualEvidence(A.AuthoredProjection, B.AuthoredProjection) && EqualEvidence(A.TypedProjection, B.TypedProjection)
  && EqualEvidence(A.MappingProjection, B.MappingProjection);
}
bool ReadStockWaitSelection(UBTTask_WaitBlackboardTime& Wait, FScan& Scan, FName& Name, int32& ID,
 const UObject*& Type, const FBlackboardKeySelector*& Selection)
{
 // Reuse the current complete scan's typed root and retained rows. Gathering
 // again would charge the same property nodes twice against its closed budget.
 const auto* Root = Scan.StockWaitSelectors.Find(&Wait);
 const auto* Property = Root ? CastField<FStructProperty>((*Root)->GetProperty()) : nullptr;
 if (!Property || Property->GetOwnerStruct() != UBTTask_WaitBlackboardTime::StaticClass()
  || Property != FindFProperty<FStructProperty>(UBTTask_WaitBlackboardTime::StaticClass(), TEXT("BlackboardKey"))
  || Property->GetOwnerProperty() != Property || Property->ArrayDim != 1
  || Property->Struct != FBlackboardKeySelector::StaticStruct() || !Property->HasAnyPropertyFlags(CPF_Edit)
  || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst)) return false;
 TArray<UObject*> Outers; (*Root)->GetOuterObjects(Outers);
 void* Address = nullptr;
 if (Outers.Num() != 1 || Outers[0] != &Wait || (*Root)->GetValueData(Address) != FPropertyAccess::Success
  || Address != Property->ContainerPtrToValuePtr<void>(&Wait)) return false;
 const auto* NameProperty = FindFProperty<FNameProperty>(FBlackboardKeySelector::StaticStruct(), TEXT("SelectedKeyName"));
 const auto* IDProperty = FindFProperty<FIntProperty>(FBlackboardKeySelector::StaticStruct(), TEXT("SelectedKeyID"));
 const auto* TypeProperty = FindFProperty<FClassProperty>(FBlackboardKeySelector::StaticStruct(), TEXT("SelectedKeyType"));
 if (!Address || !NameProperty || !IDProperty || !TypeProperty || TypeProperty->MetaClass != UBlackboardKeyType::StaticClass()
  || NameProperty->GetOwnerStruct() != FBlackboardKeySelector::StaticStruct()
  || IDProperty->GetOwnerStruct() != FBlackboardKeySelector::StaticStruct()
  || TypeProperty->GetOwnerStruct() != FBlackboardKeySelector::StaticStruct()) return false;
 // Observe the actual typed selector, including its stock filter objects. The
 // protected cache ID is read through its exact FIntProperty, never written.
 Selection = static_cast<const FBlackboardKeySelector*>(Address);
 Name = Selection->SelectedKeyName; ID = IDProperty->GetPropertyValue_InContainer(Selection);
 Type = Selection->SelectedKeyType.Get();
 return ID >= 0 && ID <= static_cast<int32>(FBlackboard::InvalidKey)
  && ID == static_cast<int32>(Selection->GetSelectedKeyID()) && Name == Wait.GetSelectedBlackboardKey();
}
}
bool HashAuthoredGraphValue(const FString& TreePath, const FAssetDocumentRuntimeValueDto& Value, FString& OutDigest, EAuthoredGraphReadbackVersion Version)
{
 if (Version != EAuthoredGraphReadbackVersion::V1 && Version != EAuthoredGraphReadbackVersion::V2) return false;
 return Digest(ArrayValue({StringValue(Version == EAuthoredGraphReadbackVersion::V2
  ? TEXT("assetdocument.ai.bt.value_or.authored.v2") : TEXT("assetdocument.ai.bt.value_or.authored.v1")),StringValue(TreePath),Value}), OutDigest);
}
bool Capture(UBehaviorTree& Tree, FInventory& Out, FString& Diagnostic, EAuthoredGraphReadbackVersion Version)
{
 FScan Scan;
 Scan.Inventory.ProjectionVersion = Version;
 if (!ScanTree(Tree, Scan)) { Out = {}; Diagnostic = Scan.Diagnostic; return false; }
 Out = MoveTemp(Scan.Inventory); Diagnostic.Reset(); return true;
}
bool InitializeInstalledStockWaitParameters(UBehaviorTree& Tree, const FInventory& Before,
 FInventory& After, FString& Diagnostic)
{
 Diagnostic = Rejected;
 if (Before.ProjectionVersion != EAuthoredGraphReadbackVersion::V2) return false;
 FScan Scan; Scan.Inventory.ProjectionVersion = EAuthoredGraphReadbackVersion::V2;
 if (!ScanTree(Tree, Scan) || !EqualInventory(Before, Scan.Inventory)) return false;
 FInventory Expected = Scan.Inventory;
 TArray<UBTTask_WaitBlackboardTime*> Initialize;
 auto* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph);
 for (UEdGraphNode* Node : Graph->Nodes)
 {
  auto* Wrapper = Cast<UBehaviorTreeGraphNode_Task>(Node);
  auto* Wait = Wrapper ? Cast<UBTTask_WaitBlackboardTime>(Wrapper->NodeInstance) : nullptr;
  if (!Wait || Wait->GetClass() != UBTTask_WaitBlackboardTime::StaticClass() || Wrapper->bInjectedNode) continue;
  const auto* Property = HiddenWaitProperty(*Wait, GET_MEMBER_NAME_CHECKED(UBTTask_Wait, WaitTime));
  if (!Property || Wait->GetTreeAsset() != &Tree || Wait->GetBlackboardAsset() != Tree.BlackboardAsset
   || !Scan.Instances.Contains(Wait)) return false;
  int32 Index = INDEX_NONE;
  for (int32 Candidate = 0; Candidate < Scan.Bound.Num(); ++Candidate)
  {
   const auto& Bound = Scan.Bound[Candidate];
   TArray<UObject*> Outers; Bound.Handle->GetOuterObjects(Outers);
   if (Bound.Handle->GetProperty() != Property || Outers.Num() != 1 || Outers[0] != Wait) continue;
   if (Index != INDEX_NONE || Bound.Leaf.Family != EFamily::Float) return false;
   Index = Candidate;
  }
  if (Index == INDEX_NONE) return false;
  FName Selected; int32 SelectedID = -1; const UObject* SelectedType = nullptr;
  const FBlackboardKeySelector* Selection = nullptr;
  UBlackboardData* BB = Tree.BlackboardAsset;
  if (!BB || !ReadStockWaitSelection(*Wait, Scan, Selected, SelectedID, SelectedType, Selection) || Selected.IsNone())
  {
   UE_LOG(LogTemp, Display, TEXT("Installed stock Wait rejected: exact selector observation or Blackboard is unavailable for %s"), *Wait->GetPathName());
   return false;
  }
  const FBlackboard::FKey ID = BB->GetKeyID(Selected);
  const FBlackboardEntry* Entry = ID != FBlackboard::InvalidKey ? BB->GetKey(ID) : nullptr;
  bool FilterAllows = Selection->AllowedTypes.IsEmpty();
  if (Entry && Entry->KeyType)
   for (const auto& Filter : Selection->AllowedTypes)
    if (Filter && Entry->KeyType->IsAllowedByFilter(Filter)) FilterAllows = true;
  if (!Entry || !Entry->KeyType || SelectedID != static_cast<int32>(ID) || SelectedType != Entry->KeyType->GetClass()
   || !FilterAllows || !Wait->WaitTime.IsCompatibleType(Entry->KeyType))
  {
   UE_LOG(LogTemp, Display, TEXT("Installed stock Wait rejected: key=%s selectedID=%d actualID=%d filter=%d type=%s"),
    *Selected.ToString(), SelectedID, static_cast<int32>(ID), FilterAllows,
    Entry && Entry->KeyType ? *Entry->KeyType->GetClass()->GetPathName() : TEXT("None"));
   Diagnostic = TEXT("AIProvider.BehaviorTree.ValueOrKeyTypeMismatch"); return false;
  }
  const FName OldKey = Scan.Bound[Index].Leaf.Key;
  if (Wait->WaitTime.GetKey() != OldKey || (!OldKey.IsNone() && OldKey != Selected)) return false;
  if (OldKey == Selected) continue;
  // Predict only this exact declaration's Key and its corresponding mapping
  // row. Canonical byte equality below preserves every other field and BB row.
  if (Expected.TypedProjection.Kind != EAssetDocumentRuntimeValueKind::Array || Expected.TypedProjection.Elements.Num() != 4
   || Expected.MappingProjection.Kind != EAssetDocumentRuntimeValueKind::Array || Expected.MappingProjection.Elements.Num() != 4) return false;
  auto& TypedRows = Expected.TypedProjection.Elements[3]; auto& MappingRows = Expected.MappingProjection.Elements[3];
  if (TypedRows.Kind != EAssetDocumentRuntimeValueKind::Array || MappingRows.Kind != EAssetDocumentRuntimeValueKind::Array
   || !TypedRows.Elements.IsValidIndex(Index) || !MappingRows.Elements.IsValidIndex(Index)) return false;
  auto& Typed = TypedRows.Elements[Index]; auto& Mapping = MappingRows.Elements[Index];
  if (Typed.Kind != EAssetDocumentRuntimeValueKind::Array || Typed.Elements.Num() != 4
   || Mapping.Kind != EAssetDocumentRuntimeValueKind::Array || Mapping.Elements.Num() != 6
   || !EqualEvidence(Typed.Elements[0], Scan.Bound[Index].Leaf.LocatorValue)
   || !EqualEvidence(Mapping.Elements[0], Scan.Bound[Index].Leaf.LocatorValue)) return false;
  auto* Key = Field(Typed.Elements[3], TEXT("Key"));
  if (!Key || Key->Kind != EAssetDocumentRuntimeValueKind::String || Key->StringValue != OldKey.ToString()) return false;
  *Key = StringValue(Selected.ToString());
  Expected.Leaves[Index].Key = Selected; Expected.Leaves[Index].Authored = Typed.Elements[3];
  Mapping.Elements[2] = StringValue(Selected.ToString()); Mapping.Elements[3] = IntegerValue(static_cast<int32>(ID));
  Mapping.Elements[4] = StringValue(Entry->KeyType->GetClass()->GetPathName());
  if (!BuildAuthoredProperties(*Entry->KeyType, Mapping.Elements[5])) return false;
  Initialize.Add(Wait);
 }
 if (!Digest(Expected.TypedProjection, Expected.TypedDigest) || !Digest(Expected.MappingProjection, Expected.MappingDigest)) return false;
 for (auto* Wait : Initialize) Wait->UBTTask_WaitBlackboardTime::InitializeFromAsset(Tree);
 FInventory Observed;
 if (!Capture(Tree, Observed, Diagnostic, EAuthoredGraphReadbackVersion::V2) || !EqualInventory(Expected, Observed))
 { Diagnostic = TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"); return false; }
 After = MoveTemp(Observed); Diagnostic.Reset(); return true;
}
bool VisitExactLeaves(UBehaviorTree& Tree, const FInventory& Expected,
 TFunctionRef<bool(FExactLeafReference&, const FLeaf&)> Visitor, FString& Diagnostic)
{
 FScan Scan;
 Scan.Inventory.ProjectionVersion = Expected.ProjectionVersion;
 if (!ScanTree(Tree, Scan)) { Diagnostic = Scan.Diagnostic; return false; }
 const FInventory& Now = Scan.Inventory;
 if (Now.TreePath != Expected.TreePath || Now.PackagePath != Expected.PackagePath
  || Now.GraphIdentity != Expected.GraphIdentity || Now.AuthoredDigest != Expected.AuthoredDigest
  || Now.TypedDigest != Expected.TypedDigest || Now.MappingDigest != Expected.MappingDigest
  || Now.FamilyCounts != Expected.FamilyCounts || Now.Leaves.Num() != Expected.Leaves.Num())
 { Diagnostic = Rejected; return false; }
 for (FBoundLeaf& Bound : Scan.Bound)
 {
  if (!WithReference(*Bound.Handle, Bound.Leaf.Family, [&](FExactLeafReference& Ref) { return Visitor(Ref, Bound.Leaf); }))
  { Diagnostic = TEXT("AIProvider.BehaviorTree.ValueOrInvalidationFailed"); return false; }
 }
 Diagnostic.Reset(); return true;
}
}
