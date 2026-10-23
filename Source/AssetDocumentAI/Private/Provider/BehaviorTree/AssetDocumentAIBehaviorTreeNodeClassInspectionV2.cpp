// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Tasks/BTTask_RunBehaviorDynamic.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "StructUtils/InstancedStruct.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAINodeClassInspectionV2
{
using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
using FValue=FAssetDocumentRuntimeValueDto;
bool Authored(const FProperty& P)
{ return P.HasAnyPropertyFlags(CPF_Edit)&&!P.HasAnyPropertyFlags(CPF_Transient|CPF_Deprecated|CPF_EditConst); }
FValue Kind(const TCHAR* Name){return ObjectValue({TEXT("kind")},{StringValue(Name)});}
bool Path(const FValue& V)
{
 if(V.Kind!=EAssetDocumentRuntimeValueKind::String||V.StringValue.IsEmpty())return false;
 const FSoftObjectPath P(V.StringValue);return P.IsValid()&&P.GetSubPathString().IsEmpty()&&P.ToString()==V.StringValue;
}
const UEnum* Enum(const FProperty& P)
{
 if(const auto* E=CastField<FEnumProperty>(&P))return E->GetEnum();
 if(const auto* B=CastField<FByteProperty>(&P))return B->Enum;
 return nullptr;
}
bool EnumValues(const UEnum& E,FValue& Out,bool Aliases=false)
{
 TMap<FString,int64> Values;
 for(int32 I=0;I<E.NumEnums();++I)
 {
  const FString Name=E.GetNameStringByIndex(I);
  if((E.GetNameStringByValue(E.GetValueByIndex(I))!=Name)!=Aliases)continue;
  if(Name.IsEmpty()||Values.Contains(Name))return false;
  Values.Add(Name,E.GetValueByIndex(I));
 }
 TArray<FString> Keys;Values.GetKeys(Keys);Keys.Sort();TArray<FValue> Rows;
 for(const auto& Key:Keys)Rows.Add(IntegerValue(Values[Key]));
 Out=ObjectValue(MoveTemp(Keys),MoveTemp(Rows));return Aliases||!Out.ObjectKeys.IsEmpty();
}
bool StructDefaults(const UScriptStruct& Type,FValue& Out)
{
 auto* Module=FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));if(!Module)return false;
 auto Storage=MakeShared<FStructOnScope>(&Type);
 auto Rows=Module->CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
 ConfigureReadOnlyValueOrRows(*Rows);Rows->SetStructure(Storage);
 TMap<FString,TSharedRef<IPropertyHandle>> Fields;int32 Budget=4096;
 TFunction<bool(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather;
 Gather=[&](const auto& Nodes)
 {
  for(const auto& Node:Nodes)
  {
   if(--Budget<0)return false;
   auto H=Node->CreatePropertyHandle();
   for(int32 D=0;H.IsValid()&&D<64;++D,H=H->GetParentHandle())
   {
    const FProperty* P=H->GetProperty();
    if(P&&P->GetOwnerProperty()==P&&Type.IsChildOf(P->GetOwnerStruct())&&IsAuthoredProperty(*H))Fields.Add(P->GetName(),H.ToSharedRef());
   }
   TArray<TSharedRef<IDetailTreeNode>> Children;Node->GetChildren(Children,true);if(!Gather(Children))return false;
  }
  return true;
 };
 if(!Gather(Rows->GetRootTreeNodes()))return false;
 for(TFieldIterator<FProperty> It(&Type);It;++It)if(Authored(**It)&&!Fields.Contains(It->GetName()))return false;
 TArray<FString> Keys;Fields.GetKeys(Keys);Keys.Sort();TArray<FValue> Values;FPropertyReadContext Context;Context.Version=EAuthoredGraphReadbackVersion::V2;
 for(const auto& Key:Keys){FValue V;if(!ReadAuthoredProperty(*Fields.FindChecked(Key),Context,1,V))return false;Values.Add(MoveTemp(V));}
 Out=ObjectValue(MoveTemp(Keys),MoveTemp(Values));return true;
}
struct FSchema
{
 int32 Remaining=4096;
 bool Fields(const UStruct& Type,const FValue* Defaults,FValue& Out,int32 Depth)
 {
  if(Depth>64||--Remaining<0)return false;
  TArray<FString> Names;
  if(Defaults)
  {
   if(Defaults->Kind!=EAssetDocumentRuntimeValueKind::Object)return false;
   Names=Defaults->ObjectKeys;
  }
  else for(TFieldIterator<FProperty> It(&Type);It;++It)if(Authored(**It))Names.Add(It->GetName());
  Names.Sort();TArray<FValue> Shapes;
  for(const auto& Name:Names)
  {
   const FProperty* P=FindFProperty<FProperty>(&Type,*Name);if(!P||!Authored(*P))return false;
   const FValue* Default=nullptr;
   if(Defaults){const int32 I=Defaults->ObjectKeys.IndexOfByKey(Name);if(I==INDEX_NONE)return false;Default=&Defaults->Elements[I];}
   FValue S;if(!Shape(*P,Default,S,Depth+1))return false;
   Shapes.Add(MoveTemp(S));
  }
  Out=ObjectValue(MoveTemp(Names),MoveTemp(Shapes));return true;
 }
 bool Shape(const FProperty& P,const FValue* Default,FValue& Out,int32 Depth,bool StaticElement=false)
 {
  if(Depth>64||--Remaining<0)return false;
  if(P.ArrayDim>1&&!StaticElement)
  {
   FValue Element;if(!Shape(P,nullptr,Element,Depth+1,true))return false;
   Out=ObjectValue({TEXT("element"),TEXT("kind")},{MoveTemp(Element),StringValue(TEXT("array"))});
  }
  else if(const auto* A=CastField<FArrayProperty>(&P))
  {
   FValue Element;if(!Shape(*A->Inner,nullptr,Element,Depth+1))return false;
   Out=ObjectValue({TEXT("element"),TEXT("kind")},{MoveTemp(Element),StringValue(TEXT("array"))});
  }
  else if(const auto* SetProperty=CastField<FSetProperty>(&P))
  {
   FValue Element;if(!Shape(*SetProperty->ElementProp,nullptr,Element,Depth+1))return false;
   Out=ObjectValue({TEXT("element"),TEXT("kind")},{MoveTemp(Element),StringValue(TEXT("set"))});
  }
  else if(const auto* M=CastField<FMapProperty>(&P))
  {
   if(!CastField<FEnumProperty>(M->KeyProp)&&!CastField<FByteProperty>(M->KeyProp)
    &&!CastField<FStrProperty>(M->KeyProp)&&!CastField<FNameProperty>(M->KeyProp))return false;
   FValue Value;if(!Shape(*M->ValueProp,nullptr,Value,Depth+1))return false;
   if(const UEnum* E=Enum(*M->KeyProp))
   {
    FValue Keys,Aliases;if(!EnumValues(*E,Keys)||!EnumValues(*E,Aliases,true))return false;
    Out=ObjectValue({TEXT("key_enum"),TEXT("kind"),TEXT("value")},{MoveTemp(Keys),StringValue(TEXT("map")),MoveTemp(Value)});
    if(!Aliases.ObjectKeys.IsEmpty()){Out.ObjectKeys.Insert(TEXT("key_enum_aliases"),1);Out.Elements.Insert(MoveTemp(Aliases),1);}
   }
   else if(CastField<FNameProperty>(M->KeyProp)||CastField<FStrProperty>(M->KeyProp))
    Out=ObjectValue({TEXT("key_kind"),TEXT("kind"),TEXT("value")},
     {StringValue(CastField<FNameProperty>(M->KeyProp)?TEXT("name"):TEXT("string")),StringValue(TEXT("map")),MoveTemp(Value)});
   else Out=ObjectValue({TEXT("kind"),TEXT("value")},{StringValue(TEXT("map")),MoveTemp(Value)});
  }
  else if(const auto* O=CastField<FObjectPropertyBase>(&P);O&&P.HasAnyPropertyFlags(CPF_InstancedReference))
   Out=ObjectValue({TEXT("declared_class_path"),TEXT("kind")},{StringValue(O->PropertyClass->GetPathName()),StringValue(TEXT("instanced_object"))});
  else if(const auto* S=CastField<FStructProperty>(&P))
  {
   if(S->Struct==FBlackboardKeySelector::StaticStruct())Out=Kind(TEXT("selector"));
   else if(S->Struct==FInstancedStruct::StaticStruct())Out=Kind(TEXT("instanced_struct"));
   else
   {
    FValue NativeDefault;
    if(!Default){if(!StructDefaults(*S->Struct,NativeDefault))return false;Default=&NativeDefault;}
    FValue Children;if(!Fields(*S->Struct,Default,Children,Depth+1))return false;
    Out=ObjectValue({TEXT("fields"),TEXT("kind"),TEXT("struct_path")},
     {MoveTemp(Children),StringValue(TEXT("struct")),StringValue(S->Struct->GetPathName())});
    // Own the temporary native default before its structure view is released.
    if(Default==&NativeDefault){Out.ObjectKeys.Insert(TEXT("default"),0);Out.Elements.Insert(NativeDefault,0);Default=nullptr;}
   }
  }
  else if(const UEnum* E=Enum(P))
  {
   FValue Values,Aliases;if(!EnumValues(*E,Values)||!EnumValues(*E,Aliases,true))return false;
   Out=ObjectValue({TEXT("kind"),TEXT("values")},{StringValue(TEXT("enum")),MoveTemp(Values)});
   if(!Aliases.ObjectKeys.IsEmpty()){Out.ObjectKeys.Insert(TEXT("aliases"),0);Out.Elements.Insert(MoveTemp(Aliases),0);}
  }
  else if(CastField<FFloatProperty>(&P)||CastField<FDoubleProperty>(&P))
   Out=ObjectValue({TEXT("kind"),TEXT("precision")},{StringValue(TEXT("number")),StringValue(CastField<FFloatProperty>(&P)?TEXT("float32"):TEXT("float64"))});
  else if(CastField<FNameProperty>(&P))Out=Kind(TEXT("name"));
  else if(CastField<FBoolProperty>(&P))
   Out=ObjectValue({TEXT("kind"),TEXT("value_type")},{StringValue(TEXT("value")),StringValue(TEXT("bool"))});
  else if(const auto* N=CastField<FNumericProperty>(&P))
  {
   const int32 Bits=N->GetElementSize()*8;
   const bool Signed=CastField<FInt8Property>(&P)||CastField<FInt16Property>(&P)||CastField<FIntProperty>(&P)||CastField<FInt64Property>(&P);
   const bool Unsigned=CastField<FByteProperty>(&P)||CastField<FUInt16Property>(&P)||CastField<FUInt32Property>(&P)||CastField<FUInt64Property>(&P);
   if(!N->IsInteger()||(!Signed&&!Unsigned)||(Bits!=8&&Bits!=16&&Bits!=32&&Bits!=64))return false;
   Out=ObjectValue({TEXT("integer_bits"),TEXT("integer_signed"),TEXT("kind"),TEXT("value_type")},
    {IntegerValue(Bits),BooleanValue(Signed),StringValue(TEXT("value")),StringValue(TEXT("integer"))});
  }
  else if(CastField<FStrProperty>(&P)||CastField<FTextProperty>(&P))
   Out=ObjectValue({TEXT("kind"),TEXT("value_type")},
    {StringValue(TEXT("value")),StringValue(CastField<FStrProperty>(&P)?TEXT("string"):TEXT("text"))});
  else if(const auto* ObjectProperty=CastField<FObjectPropertyBase>(&P);ObjectProperty&&(CastField<FObjectProperty>(&P)||CastField<FSoftObjectProperty>(&P)))
  {
   const UClass* Declared=ObjectProperty->PropertyClass;
   bool ClassReference=Declared&&Declared->IsChildOf(UClass::StaticClass());
   const TCHAR* Constraint=TEXT("object_class");
   if(const auto* ClassProperty=CastField<FClassProperty>(&P)){Declared=ClassProperty->MetaClass;ClassReference=true;Constraint=TEXT("represented_class");}
   else if(const auto* SoftClassProperty=CastField<FSoftClassProperty>(&P)){Declared=SoftClassProperty->MetaClass;ClassReference=true;Constraint=TEXT("represented_class");}
   if(!Declared)return false;
   Out=ObjectValue({TEXT("declared_class_path"),TEXT("kind"),TEXT("reference_constraint"),TEXT("reference_kind"),TEXT("reference_soft"),TEXT("value_type")},
    {StringValue(Declared->GetPathName()),StringValue(TEXT("value")),StringValue(Constraint),StringValue(ClassReference?TEXT("ClassRef"):TEXT("AssetRef")),BooleanValue(CastField<FSoftObjectProperty>(&P)!=nullptr),StringValue(TEXT("reference"))});
  }
  else return false;
  // These exact declared task fields carry subtree semantics. Ordinary object
  // references do not acquire binding policy from their spelling or value.
  if ((P.GetOwnerStruct()==UBTTask_RunBehavior::StaticClass()&&P.GetFName()==TEXT("BehaviorAsset"))
   ||(P.GetOwnerStruct()==UBTTask_RunBehaviorDynamic::StaticClass()&&P.GetFName()==TEXT("DefaultBehaviorAsset")))
  {
   Out.ObjectKeys.Insert(TEXT("binding_kind"),0);
   Out.Elements.Insert(StringValue(P.GetOwnerStruct()==UBTTask_RunBehavior::StaticClass()
    ?TEXT("static_subtree"):TEXT("dynamic_subtree")),0);
  }
  if(Default)
  {
   int32 Index=0;while(Index<Out.ObjectKeys.Num()&&Out.ObjectKeys[Index].Compare(TEXT("default"),ESearchCase::CaseSensitive)<0)++Index;
   Out.ObjectKeys.Insert(TEXT("default"),Index);Out.Elements.Insert(*Default,Index);
  }
  return true;
 }
};
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeNodeClassInspectV2(
 const FAssetDocumentPrimitiveInvocation& Invocation,IAssetDocumentHandleResolver&,IAssetDocumentAtomicTransaction*)
{
 using namespace AssetDocumentAINodeClassInspectionV2;
 auto Fail=[](const TCHAR* Code,const TCHAR* Message){return MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed,Code,Message);};
 if(!IsInGameThread()||!GIsEditor||IsRunningCommandlet()||IsGarbageCollecting()
  ||(Invocation.Phase!=EAssetDocumentRuntimeRequestPhase::Validate&&Invocation.Phase!=EAssetDocumentRuntimeRequestPhase::Preflight
   &&Invocation.Phase!=EAssetDocumentRuntimeRequestPhase::Apply&&Invocation.Phase!=EAssetDocumentRuntimeRequestPhase::Repair&&Invocation.Phase!=EAssetDocumentRuntimeRequestPhase::Verify))
  return Fail(TEXT("AIProvider.InvalidContext"),TEXT("Class inspection requires its admitted Editor phase."));
 const auto& V=Invocation.ValueArguments;
 if(Invocation.Version!=2||Invocation.PrimitiveId!=TEXT("ai.behavior_tree.node_class.inspect.v2")
  ||!Invocation.HandleArguments.IsEmpty()||!IsAssetDocumentAICanonicalRuntimeValue(V)
  ||V.Kind!=EAssetDocumentRuntimeValueKind::Object||V.ObjectKeys!=TArray<FString>{TEXT("expected_base_class_path"),TEXT("node_class_path"),TEXT("struct_type_paths")}
  ||V.Elements.Num()!=3||!Path(V.Elements[0])||!Path(V.Elements[1])||V.Elements[2].Kind!=EAssetDocumentRuntimeValueKind::Array)
  return Fail(TEXT("AIProvider.InvalidValueShape"),TEXT("Class inspection requires its exact three-field schema."));
 UClass* Base=FSoftClassPath(V.Elements[0].StringValue).ResolveClass();UClass* Class=FSoftClassPath(V.Elements[1].StringValue).ResolveClass();
 if(!Class||!Base||Class->GetPathName()!=V.Elements[1].StringValue||Base->GetPathName()!=V.Elements[0].StringValue)
  return Fail(TEXT("AIProvider.BehaviorTree.NodeClassUnavailable"),TEXT("The exact node and base classes must already be loaded."));
 const bool NodeFamily=Class->IsChildOf(UBTTaskNode::StaticClass())||Class->IsChildOf(UBTCompositeNode::StaticClass())
  ||Class->IsChildOf(UBTDecorator::StaticClass())||Class->IsChildOf(UBTService::StaticClass());
 const bool InlineClass=Base->IsChildOf(UObject::StaticClass())&&Class->HasAnyClassFlags(CLASS_EditInlineNew);
 if(Class->HasAnyClassFlags(CLASS_Abstract))
  return Fail(TEXT("AIProvider.BehaviorTree.NodeClassAbstract"),TEXT("The actual class has CLASS_Abstract."));
 if(Class->HasAnyClassFlags(CLASS_Deprecated|CLASS_NewerVersionExists))
  return Fail(TEXT("AIProvider.BehaviorTree.NodeClassDeprecated"),TEXT("The actual class is deprecated or superseded."));
 if(!Class->IsChildOf(Base)||Class->HasAnyClassFlags(CLASS_Abstract|CLASS_Deprecated|CLASS_NewerVersionExists)
  ||(!NodeFamily&&!InlineClass))
  return Fail(TEXT("AIProvider.BehaviorTree.NodeClassRejected"),TEXT("The node class is not a concrete admitted BehaviorTree family."));
 UObject* CDO=Class->GetDefaultObject(false);FValue Defaults,Properties,Structs;FSchema Schema;
 if(!CDO||!BuildAuthoredProperties(*CDO,Defaults,nullptr,EAuthoredGraphReadbackVersion::V2)||!Schema.Fields(*Class,&Defaults,Properties,0))
  return Fail(TEXT("AIProvider.BehaviorTree.PropertyEvidenceUnavailable"),TEXT("Complete authored defaults or typed metadata are unavailable."));
 Structs.Kind=EAssetDocumentRuntimeValueKind::Object;
 for(const auto& S:V.Elements[2].Elements)
 {
  if(!Path(S)||(!Structs.ObjectKeys.IsEmpty()&&Structs.ObjectKeys.Last().Compare(S.StringValue,ESearchCase::CaseSensitive)>=0))
   return Fail(TEXT("AIProvider.InvalidValueShape"),TEXT("Struct paths must be unique and in canonical order."));
  const auto* Type=Cast<UScriptStruct>(FSoftObjectPath(S.StringValue).ResolveObject());FValue Fields,NativeDefaults;
  if(!Type||Type->GetPathName()!=S.StringValue)
   return Fail(TEXT("AIProvider.BehaviorTree.PropertyEvidenceUnavailable"),*FString::Printf(TEXT("Explicit struct is not loaded: %s."),*S.StringValue));
  if(!StructDefaults(*Type,NativeDefaults))
   return Fail(TEXT("AIProvider.BehaviorTree.PropertyEvidenceUnavailable"),*FString::Printf(TEXT("Native struct default readback failed: %s."),*S.StringValue));
  if(!Schema.Fields(*Type,&NativeDefaults,Fields,0))
   return Fail(TEXT("AIProvider.BehaviorTree.PropertyEvidenceUnavailable"),*FString::Printf(TEXT("Typed struct field schema failed: %s."),*S.StringValue));
  Structs.ObjectKeys.Add(S.StringValue);Structs.Elements.Add(MoveTemp(Fields));
 }
 FAssetDocumentPrimitiveOutcome Result;
 const TCHAR* Family=Class->IsChildOf(UBTCompositeNode::StaticClass())?TEXT("composite")
  :Class->IsChildOf(UBTTaskNode::StaticClass())?TEXT("task")
  :Class->IsChildOf(UBTDecorator::StaticClass())?TEXT("decorator")
  :Class->IsChildOf(UBTService::StaticClass())?TEXT("service"):TEXT("inline_object");
 FValue PinSemantic;PinSemantic.Kind=EAssetDocumentRuntimeValueKind::Boolean;
 PinSemantic.BooleanValue=Class->IsChildOf(UBTComposite_SimpleParallel::StaticClass());
 Result.Value=ObjectValue({TEXT("authored_defaults"),TEXT("node_class_path"),TEXT("node_family"),
  TEXT("pin_semantic_children"),TEXT("property_schema"),TEXT("struct_schemas")},
  {MoveTemp(Defaults),StringValue(Class->GetPathName()),StringValue(Family),MoveTemp(PinSemantic),MoveTemp(Properties),MoveTemp(Structs)});
 if(!IsAssetDocumentAICanonicalRuntimeValue(Result.Value))return Fail(TEXT("AIProvider.BehaviorTree.PropertyEvidenceUnavailable"),TEXT("Class evidence exceeds the canonical value budget."));
 Result.Status=EAssetDocumentPrimitiveOutcomeStatus::Succeeded;return Result;
}
