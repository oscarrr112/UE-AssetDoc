// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "Tests/AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "Tests/Fixtures/AssetDocumentAIReadbackTestTypes.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"

namespace AssetDocumentAINodeClassV2Tests
{
const FAssetDocumentRuntimeValueDto* Field(const FAssetDocumentRuntimeValueDto& V,const TCHAR* Name)
{const int32 I=V.ObjectKeys.IndexOfByKey(Name);return V.Elements.IsValidIndex(I)?&V.Elements[I]:nullptr;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAINodeClassV2Test,
 "UEAssetDoc.AIProvider.BehaviorTree.NodeClassInspectionV2.TypedDefaults",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAINodeClassV2Test::RunTest(const FString&)
{
 using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
 using namespace AssetDocumentAINodeClassV2Tests;
 for(UClass* Class:{UBTDecorator_Blackboard::StaticClass(),UAssetDocumentAIValueOrTask::StaticClass(),UAssetDocumentAIInstallOwnedChild::StaticClass(),UAssetDocumentAIInstallFamiliesTask::StaticClass()})
 {
  UObject* CDO=Class->GetDefaultObject();FAssetDocumentRuntimeValueDto Before;
  if(!TestTrue(TEXT("native full CDO defaults available"),BuildAuthoredProperties(*CDO,Before,nullptr,EAuthoredGraphReadbackVersion::V2)))return false;
  TArray<FString> Paths={FAssetDocumentAIReadbackFields::StaticStruct()->GetPathName(),FAssetDocumentAIValueOrContainers::StaticStruct()->GetPathName()};Paths.Sort();
  TArray<FAssetDocumentRuntimeValueDto> StructPaths;for(const auto& P:Paths)StructPaths.Add(StringValue(P));
  FAssetDocumentPrimitiveInvocation I;I.PrimitiveId=TEXT("ai.behavior_tree.node_class.inspect.v2");I.Version=2;
  I.RequestId=FGuid::NewGuid();I.TransactionId=FGuid::NewGuid();I.RuntimeRevision=1;I.Phase=EAssetDocumentRuntimeRequestPhase::Preflight;
  I.ValueArguments=ObjectValue({TEXT("expected_base_class_path"),TEXT("node_class_path"),TEXT("struct_type_paths")},
   {StringValue(Class==UAssetDocumentAIInstallOwnedChild::StaticClass()?TEXT("/Script/CoreUObject.Object"):TEXT("/Script/AIModule.BTNode")),StringValue(Class->GetPathName()),ArrayValue(MoveTemp(StructPaths))});
  AssetDocumentAIBehaviorTreeProviderTestSupport::FResolveExistingHandleResolver Resolver;
  const auto O=CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,Resolver,nullptr);
  for(const auto& D:O.Diagnostics)AddInfo(D.Code+TEXT(": ")+D.Message);
  if(!TestEqual(TEXT("real class V2 succeeds"),O.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded))return false;
  TestTrue(TEXT("metadata has no handle or mutation results"),O.HandleResults.IsEmpty()&&O.ChangedSet.IsEmpty());
  const auto* Defaults=Field(O.Value,TEXT("authored_defaults"));const auto* Schema=Field(O.Value,TEXT("property_schema"));const auto* Structs=Field(O.Value,TEXT("struct_schemas"));
  if(!TestTrue(TEXT("complete output fields"),Defaults&&Schema&&Structs))return false;
  TArray<uint8> BeforeBytes,ActualBytes;EncodeAuthoredCanonicalValue(Before,BeforeBytes);EncodeAuthoredCanonicalValue(*Defaults,ActualBytes);
  TestEqual(TEXT("class defaults retain original D2 bytes"),ActualBytes,BeforeBytes);
  if(Class==UBTDecorator_Blackboard::StaticClass())
  {
   const auto* Selector=Field(*Schema,TEXT("BlackboardKey"));const auto* Arithmetic=Field(*Schema,TEXT("ArithmeticOperation"));
   if(!TestTrue(TEXT("selector and enum fields have explicit evidence"),Selector&&Arithmetic))return false;
   TestEqual(TEXT("selector cannot masquerade as generic struct"),Field(*Selector,TEXT("kind"))->StringValue,FString(TEXT("selector")));
   TestEqual(TEXT("enum retains label mapping kind"),Field(*Arithmetic,TEXT("kind"))->StringValue,FString(TEXT("enum")));
   TestTrue(TEXT("enum labels available"),Field(*Arithmetic,TEXT("values"))->ObjectKeys.Num()>1);
  }
  if(Class==UAssetDocumentAIInstallFamiliesTask::StaticClass())
  {
   const auto* Nested=Field(*Schema,TEXT("Nested"));if(!TestNotNull(TEXT("instanced UObject has explicit metadata"),Nested))return false;
   TestEqual(TEXT("instanced object distinct from reference"),Field(*Nested,TEXT("kind"))->StringValue,FString(TEXT("instanced_object")));
   TestEqual(TEXT("declared exact UObject class is preserved"),Field(*Nested,TEXT("declared_class_path"))->StringValue,UAssetDocumentAIInstallOwnedChild::StaticClass()->GetPathName());
  }
  const auto* Simple=Field(*Structs,*FAssetDocumentAIReadbackFields::StaticStruct()->GetPathName());
  const auto* Containers=Field(*Structs,*FAssetDocumentAIValueOrContainers::StaticStruct()->GetPathName());
  if(!TestTrue(TEXT("explicit loaded structs inspected"),Simple&&Containers))return false;
  TestEqual(TEXT("actual native constructor default is published"),Field(*Field(*Simple,TEXT("Count")),TEXT("default"))->IntegerValue,int64(37));
  TestNull(TEXT("transient metadata excluded"),Field(*Simple,TEXT("Cache")));
  const auto* Element=Field(*Field(*Containers,TEXT("Array")),TEXT("element"));
  if(!TestNotNull(TEXT("empty container still has element schema"),Element))return false;
  const auto* ElementFields=Field(*Element,TEXT("fields"));
  TestEqual(TEXT("omitted new container field has actual native default"),Field(*Field(*ElementFields,TEXT("Identity")),TEXT("default"))->IntegerValue,int64(0));
  TestEqual(TEXT("map kind"),Field(*Field(*Containers,TEXT("Map")),TEXT("kind"))->StringValue,FString(TEXT("map")));
  const auto* MapKeyKind=Field(*Field(*Containers,TEXT("Map")),TEXT("key_kind"));
  if(!TestNotNull(TEXT("map key type is explicit"),MapKeyKind))return false;
  TestEqual(TEXT("native FName map key is distinguished from FString"),MapKeyKind->StringValue,FString(TEXT("name")));
  FAssetDocumentRuntimeValueDto After;TestTrue(TEXT("CDO remains readable"),BuildAuthoredProperties(*CDO,After,nullptr,EAuthoredGraphReadbackVersion::V2));ActualBytes.Reset();EncodeAuthoredCanonicalValue(After,ActualBytes);TestEqual(TEXT("inspection does not modify CDO"),ActualBytes,BeforeBytes);
  auto Invalid=I;Invalid.ValueArguments.Elements[1]=StringValue(TEXT("/Script/AIModule.NotLoadedBehaviorNode"));
  TestNotEqual(TEXT("unknown class is not loaded as fallback"),CreateAssetDocumentAIPrimitiveProvider()->Invoke(Invalid,Resolver,nullptr).Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 }
 return !HasAnyErrors();
}
#endif
