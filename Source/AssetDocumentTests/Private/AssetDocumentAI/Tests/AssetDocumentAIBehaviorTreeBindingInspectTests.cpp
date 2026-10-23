// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "Provider/AssetDocumentAIPrimitiveHandler.h"
#include "Tests/AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "Tests/Fixtures/AssetDocumentAIReadbackTestTypes.h"
#include "Tests/Fixtures/AssetDocumentAISelectorInventoryTestTypes.h"
#include "Tests/AssetDocumentAIBlackboardCanonicalStructTestTypes.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Tasks/BTTask_RunBehaviorDynamic.h"
#include "AIGraphTypes.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace AssetDocumentAIBindingInspectTests
{
using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
using namespace AssetDocumentAIBehaviorTreeProviderTestSupport;
using FValue = FAssetDocumentRuntimeValueDto;
const FValue* Field(const FValue& V, const TCHAR* Name)
{ const int32 I = V.ObjectKeys.IndexOfByKey(Name); return V.Elements.IsValidIndex(I) ? &V.Elements[I] : nullptr; }
FValue* Field(FValue& V, const TCHAR* Name)
{ const int32 I = V.ObjectKeys.IndexOfByKey(Name); return V.Elements.IsValidIndex(I) ? &V.Elements[I] : nullptr; }
FValue Member(const TCHAR* Name)
{ return ObjectValue({TEXT("kind"), TEXT("name")}, {StringValue(TEXT("field")), StringValue(Name)}); }
FValue Selector(TArray<FValue> Steps, const TCHAR* Key)
{ return ObjectValue({TEXT("kind"), TEXT("locator"), TEXT("selected_key")}, {StringValue(TEXT("selector")), ArrayValue(MoveTemp(Steps)), StringValue(Key)}); }
FValue ValueOr(const TCHAR* Name, const TCHAR* Key, FValue Authored = NullValue())
{
 return ObjectValue({TEXT("authored_value_or_null"), TEXT("kind"), TEXT("locator"), TEXT("selected_key")},
  {MoveTemp(Authored), StringValue(TEXT("value_or")), ArrayValue({Member(Name)}), StringValue(Key)});
}
FValue Subtree(bool Static, UBehaviorTree* Tree)
{
 return ObjectValue({TEXT("kind"), TEXT("locator"), TEXT("subtree_path_or_null")},
  {StringValue(Static ? TEXT("static_subtree") : TEXT("dynamic_subtree")),
   ArrayValue({Member(Static ? TEXT("BehaviorAsset") : TEXT("DefaultBehaviorAsset"))}), Tree ? StringValue(Tree->GetPathName()) : NullValue()});
}
FAssetDocumentPrimitiveInvocation ClassRequest(UClass& Class, UBlackboardData* BB, TArray<FValue> Queries)
{
 FAssetDocumentPrimitiveInvocation I;
 I.PrimitiveId = TEXT("ai.behavior_tree.binding.inspect.v1"); I.Version = 1;
 I.RequestId = FGuid::NewGuid(); I.TransactionId = FGuid::NewGuid(); I.RuntimeRevision = 1;
 I.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
 I.ValueArguments = ObjectValue({TEXT("blackboard_path_or_null"), TEXT("queries"), TEXT("source")},
  {BB ? StringValue(BB->GetPathName()) : NullValue(), ArrayValue(MoveTemp(Queries)),
   ObjectValue({TEXT("kind"), TEXT("node_class_path")}, {StringValue(TEXT("class_defaults")), StringValue(Class.GetPathName())})});
 return I;
}
FAssetDocumentPrimitiveOutcome Invoke(FAutomationTestBase& T, const FAssetDocumentPrimitiveInvocation& I, FResolveExistingHandleResolver& Resolver)
{
 const auto Out = CreateAssetDocumentAIPrimitiveProvider()->Invoke(I, Resolver, nullptr);
 if (Out.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
  for (const auto& D : Out.Diagnostics) T.AddInfo(D.Code + TEXT(": ") + D.Message);
 return Out;
}
bool Snapshot(UObject& Object, TArray<uint8>& Bytes)
{
 Bytes.Reset(); // The canonical encoder appends; each snapshot must replace the previous sample.
 FValue Value;
 return BuildAuthoredProperties(Object, Value, nullptr, EAuthoredGraphReadbackVersion::V2)
  && EncodeAuthoredCanonicalValue(Value, Bytes);
}
UEdGraphPin* Output(UEdGraphNode& N)
{ for (auto* P : N.Pins) if (P && P->Direction == EGPD_Output) return P; return nullptr; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBindingSelectorTest,
 "UEAssetDoc.AIProvider.BehaviorTree.BindingInspect.RealSelectorPoliciesAndTypedLocators",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBindingSelectorTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBindingInspectTests;
 if (!TestTrue(TEXT("binding inspection requires a valid complete AI assembly"), IsAssetDocumentAIPrimitiveAssemblyValid())) return false;
 const auto Provider = CreateAssetDocumentAIPrimitiveProvider();
 if (!TestEqual(TEXT("registered AI provider is available in the normal Editor"), Provider->InspectAvailability().State,
  EAssetDocumentProviderAvailabilityState::Available)) return false;
 UClass* Class = LoadClass<UBTTaskNode>(nullptr, TEXT("/Script/AssetDocumentTests.AssetDocumentSelectorTaskTestNode"));
 if (!TestNotNull(TEXT("actual selector fixture"), Class)) return false;
 UObject* CDO = Class->GetDefaultObject(); TArray<uint8> Before, After;
 if (!TestTrue(TEXT("CDO authored preimage"), Snapshot(*CDO, Before))) return false;
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/AssetDocumentAI/BindingSelector_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
 auto* BB = NewObject<UBlackboardData>(Package.Get(), TEXT("BB"), RF_Public);
 BB->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("Actor"))->BaseClass = AActor::StaticClass();
 BB->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("Broad"))->BaseClass = UObject::StaticClass();
 BB->UpdatePersistentKey<UBlackboardKeyType_Vector>(TEXT("WrongType"));
 const auto* Dynamic = FindFProperty<FStructProperty>(Class, TEXT("DynamicValue"))->ContainerPtrToValuePtr<FInstancedStruct>(CDO);
 const FString StructPath = Dynamic->GetScriptStruct()->GetPathName();
 const auto DynamicStep = ObjectValue({TEXT("kind"), TEXT("struct_path")}, {StringValue(TEXT("instanced_struct")), StringValue(StructPath)});
 TArray<FValue> Queries = {
  Selector({Member(TEXT("DirectSelector"))}, TEXT("Actor")),
  Selector({Member(TEXT("DirectSelector"))}, TEXT("WrongType")),
  Selector({Member(TEXT("DirectSelector"))}, TEXT("Broad")),
  Selector({Member(TEXT("DirectSelector"))}, TEXT("Missing")),
  Selector({Member(TEXT("DirectSelector"))}, TEXT("")),
  Selector({Member(TEXT("OptionalSelector"))}, TEXT("")),
  Selector({Member(TEXT("NestedValue")), Member(TEXT("Selector"))}, TEXT("Actor")),
  Selector({Member(TEXT("DynamicValue")), DynamicStep, Member(TEXT("Selector"))}, TEXT("Actor")),
  Selector({Member(TEXT("ArrayValues")), ObjectValue({TEXT("index"), TEXT("kind")}, {IntegerValue(3), StringValue(TEXT("array_element"))}), Member(TEXT("Selector"))}, TEXT("Actor")),
  Selector({Member(TEXT("MapValues")), ObjectValue({TEXT("key"), TEXT("kind")}, {StringValue(TEXT("New")), StringValue(TEXT("map_value"))}), Member(TEXT("Selector"))}, TEXT("Actor")),
  Selector({Member(TEXT("SetValues")), ObjectValue({TEXT("kind"), TEXT("value")}, {StringValue(TEXT("set_element")), ObjectValue({TEXT("Identity")}, {StringValue(TEXT("New"))})}), Member(TEXT("Selector"))}, TEXT("Actor"))};
 auto I = ClassRequest(*Class, BB, MoveTemp(Queries)); FResolveExistingHandleResolver Resolver;
 Package->SetDirtyFlag(false);
 for (const auto Phase : {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify})
 {
  I.Phase = Phase; const auto Out = Invoke(*this, I, Resolver);
  if (!TestEqual(TEXT("all real read-only phases accept typed evidence"), Out.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  const auto* Rows = Field(Out.Value, TEXT("evidence"));
  if (!TestTrue(TEXT("query order and cardinality retained"), Rows && Rows->Elements.Num() == 11)) return false;
  TestTrue(TEXT("real Actor filter passes"), Field(Rows->Elements[0], TEXT("filter_passes"))->BooleanValue);
  TestFalse(TEXT("real WrongType KeyType filter rejects"), Field(Rows->Elements[1], TEXT("filter_passes"))->BooleanValue);
  TestFalse(TEXT("ordinary selector is not an enum-decorator semantic surface"), Field(Rows->Elements[0], TEXT("blackboard_decorator_selector"))->BooleanValue);
  TestTrue(TEXT("wrong type key still exists"), Field(Rows->Elements[1], TEXT("key_exists"))->BooleanValue);
  TestFalse(TEXT("real broad Object BaseClass rejects"), Field(Rows->Elements[2], TEXT("filter_passes"))->BooleanValue);
  TestFalse(TEXT("missing key existence is evidence"), Field(Rows->Elements[3], TEXT("key_exists"))->BooleanValue);
  // A missing key brings the effective Blackboard's key names (the close ones name the fix).
  const auto* Keys = Field(Out.Value, TEXT("blackboard_keys"));
  if (TestTrue(TEXT("an unknown key lists the Blackboard's keys"), Keys && Keys->Kind == EAssetDocumentRuntimeValueKind::Array))
  {
   TArray<FString> Names;
   for (const auto& Key : Keys->Elements) Names.Add(Key.StringValue);
   TestTrue(TEXT("the Blackboard's own keys"), Names.Contains(TEXT("Actor")) && Names.Contains(TEXT("Broad")) && Names.Contains(TEXT("WrongType")));
  }
  TestFalse(TEXT("required None policy retained"), Field(Rows->Elements[4], TEXT("none_allowed"))->BooleanValue);
  TestTrue(TEXT("optional None policy retained"), Field(Rows->Elements[5], TEXT("none_allowed"))->BooleanValue);
  for (int32 N = 6; N < 11; ++N)
   TestTrue(TEXT("native nested/container constructors retain actual filter"), Field(Rows->Elements[N], TEXT("filter_passes"))->BooleanValue);
  TestTrue(TEXT("no handles or mutations returned"), Out.HandleResults.IsEmpty() && Out.ChangedSet.IsEmpty());
 }
 TestTrue(TEXT("CDO readable after inspection"), Snapshot(*CDO, After)); TestEqual(TEXT("CDO canonical bytes unchanged"), After, Before);
 TestFalse(TEXT("fixture package remains clean"), Package->IsDirty());
 auto Invalid = I; Invalid.ValueArguments.Elements[1] = ArrayValue({Selector({Member(TEXT("DirectSelector")), DynamicStep}, TEXT("Actor"))});
 TestNotEqual(TEXT("dynamic struct step cannot cross a selector declaration"), Invoke(*this, Invalid, Resolver).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 Invalid = I; Invalid.ValueArguments.Elements[1] = ArrayValue({Selector({Member(TEXT("NonAuthored"))}, TEXT("Actor"))});
 TestNotEqual(TEXT("unknown/non-authored field locator rejected"), Invoke(*this, Invalid, Resolver).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 Invalid = I; Invalid.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 TestNotEqual(TEXT("Apply cannot masquerade as read-only binding inspection"), Invoke(*this, Invalid, Resolver).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 auto* FixedCDO = GetMutableDefault<UAssetDocumentAISelectorFixedArrayTask>();
 UAssetDocumentAISelectorInlineObject::StaticClass()->GetDefaultObject();
 const auto Inline = ObjectValue({TEXT("class_path"), TEXT("kind")}, {StringValue(UAssetDocumentAISelectorInlineObject::StaticClass()->GetPathName()), StringValue(TEXT("instanced_object"))});
 Before.Reset(); After.Reset();
 if (!TestTrue(TEXT("fixed/inline CDO preimage"), Snapshot(*FixedCDO, Before))) return false;
 auto FixedI = ClassRequest(*FixedCDO->GetClass(), BB, {
  Selector({Member(TEXT("Fixed")), ObjectValue({TEXT("index"), TEXT("kind")}, {IntegerValue(1), StringValue(TEXT("array_element"))})}, TEXT("Actor")),
  Selector({Member(TEXT("First")), Inline, Member(TEXT("Selector"))}, TEXT("Actor"))});
 const auto FixedOut = Invoke(*this, FixedI, Resolver);
 if (!TestEqual(TEXT("declared fixed-array and exact inline class policy reads"), FixedOut.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 for (const auto& Row : Field(FixedOut.Value, TEXT("evidence"))->Elements)
  TestTrue(TEXT("fixed/inline unfiltered actual selector accepts existing key"), Field(Row, TEXT("filter_passes"))->BooleanValue);
 TestNull(TEXT("known keys bring no key list"), Field(FixedOut.Value, TEXT("blackboard_keys")));
 FixedI.ValueArguments.Elements[1].Elements[1].Elements[1].Elements[1].Elements[0] = StringValue(UAssetDocumentAIInstallOwnedChild::StaticClass()->GetPathName());
 TestNotEqual(TEXT("inline class cannot cross declared base"), Invoke(*this, FixedI, Resolver).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestTrue(TEXT("fixed/inline CDO still readable"), Snapshot(*FixedCDO, After)); TestEqual(TEXT("read-only inline lookup did not allocate or modify task's First object"), After, Before);
 TestNull(TEXT("null inline source property remains null"), FixedCDO->First.Get());
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBindingValueOrTest,
 "UEAssetDoc.AIProvider.BehaviorTree.BindingInspect.ElevenRealValueOrFamiliesAndIsolation",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBindingValueOrTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBindingInspectTests;
 auto* CDO = GetMutableDefault<UAssetDocumentAIInstallFamiliesTask>();
 FValue Defaults; TArray<uint8> Before, After;
 if (!TestTrue(TEXT("eleven actual CDO leaf defaults"), BuildAuthoredProperties(*CDO, Defaults, nullptr, EAuthoredGraphReadbackVersion::V2))
  || !TestTrue(TEXT("exact CDO preimage"), Snapshot(*CDO, Before))) return false;
 const UObject* NestedBefore = CDO->Nested; const int32 InlineBefore = CDO->Nested->AuthoredValue;
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/AssetDocumentAI/BindingValueOr_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
 auto* BB = NewObject<UBlackboardData>(Package.Get(), TEXT("BB"), RF_Public);
 BB->UpdatePersistentKey<UBlackboardKeyType_Bool>(TEXT("Bool"));
 BB->UpdatePersistentKey<UBlackboardKeyType_Class>(TEXT("Class"))->BaseClass = AActor::StaticClass();
 BB->UpdatePersistentKey<UBlackboardKeyType_Enum>(TEXT("Enum"))->EnumType = StaticEnum<EAssetDocumentAIReadbackEnum>();
 BB->UpdatePersistentKey<UBlackboardKeyType_Float>(TEXT("Float"));
 BB->UpdatePersistentKey<UBlackboardKeyType_Int>(TEXT("Int32"));
 BB->UpdatePersistentKey<UBlackboardKeyType_Name>(TEXT("Name"));
 BB->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("Object"))->BaseClass = AActor::StaticClass();
 BB->UpdatePersistentKey<UBlackboardKeyType_Rotator>(TEXT("Rotator"));
 BB->UpdatePersistentKey<UBlackboardKeyType_String>(TEXT("String"));
 BB->UpdatePersistentKey<UBlackboardKeyType_Struct>(TEXT("Struct"))->DefaultValue.InitializeAs<FAssetDocumentAIReadbackFields>();
 BB->UpdatePersistentKey<UBlackboardKeyType_Struct>(TEXT("StructInline"))->DefaultValue.InitializeAs<FBlackboardEntry>();
 BB->UpdatePersistentKey<UBlackboardKeyType_Vector>(TEXT("Vector"));
 const TCHAR* Names[] = {TEXT("Bool"), TEXT("Class"), TEXT("Enum"), TEXT("Float"), TEXT("Int32"), TEXT("Name"), TEXT("Object"), TEXT("Rotator"), TEXT("String"), TEXT("Struct"), TEXT("Vector")};
 TArray<FValue> Queries;
 for (const TCHAR* Name : Names)
 {
  const FString FieldName = FString(Name) + TEXT("Value"); FValue Authored = *Field(Defaults, *FieldName);
  *Field(Authored, TEXT("Key")) = StringValue(Name);
  if (FString(Name) == TEXT("Enum"))
  {
   *Field(Authored, TEXT("EnumType")) = ObjectValue({TEXT("Kind"), TEXT("Path")}, {StringValue(TEXT("AssetRef")), StringValue(StaticEnum<EAssetDocumentAIReadbackEnum>()->GetPathName())});
   *Field(Authored, TEXT("DefaultValue")) = IntegerValue(3);
   *Field(Authored, TEXT("NativeEnumTypeName")) = StringValue(TEXT("EAssetDocumentAIReadbackEnum"));
  }
  if (FString(Name) == TEXT("Struct"))
   *Field(Authored, TEXT("DefaultValue")) = ObjectValue({TEXT("Properties"), TEXT("Struct")},
    {ObjectValue({TEXT("Count"), TEXT("Label")}, {IntegerValue(37), StringValue(TEXT("isolated"))}), StringValue(FAssetDocumentAIReadbackFields::StaticStruct()->GetPathName())});
  if (FString(Name) == TEXT("Class") || FString(Name) == TEXT("Object"))
   *Field(Authored, TEXT("BaseClass")) = ObjectValue({TEXT("Kind"), TEXT("Path")}, {StringValue(TEXT("ClassRef")), StringValue(AActor::StaticClass()->GetPathName())});
  Queries.Add(ValueOr(*FieldName, Name, MoveTemp(Authored)));
 }
 Queries.Add(ValueOr(TEXT("BoolValue"), TEXT("Vector")));
 Queries.Add(ValueOr(TEXT("EnumValue"), TEXT("Enum"))); // Retained null EnumType must not inherit the authored query above.
 Queries.Add(ValueOr(TEXT("StructValue"), TEXT("Struct"))); // Retained null Struct type must likewise remain incompatible.
 FValue WrongClass = *Field(Defaults, TEXT("ObjectValue"));
 *Field(WrongClass, TEXT("BaseClass")) = ObjectValue({TEXT("Kind"), TEXT("Path")}, {StringValue(TEXT("ClassRef")), StringValue(UBehaviorTree::StaticClass()->GetPathName())});
 Queries.Add(ValueOr(TEXT("ObjectValue"), TEXT("Object"), MoveTemp(WrongClass)));
 FValue WrongEnum = *Field(Defaults, TEXT("EnumValue"));
 *Field(WrongEnum, TEXT("EnumType")) = ObjectValue({TEXT("Kind"), TEXT("Path")}, {StringValue(TEXT("AssetRef")), StringValue(StaticEnum<EAssetDocumentAIBlackboardCanonicalStructTestEnum>()->GetPathName())});
 Queries.Add(ValueOr(TEXT("EnumValue"), TEXT("Enum"), MoveTemp(WrongEnum)));
 FValue WrongStruct = *Field(Defaults, TEXT("StructValue"));
 *Field(WrongStruct, TEXT("DefaultValue")) = ObjectValue({TEXT("Properties"), TEXT("Struct")},
  {ObjectValue({}, {}), StringValue(FAssetDocumentAIReadbackEmpty::StaticStruct()->GetPathName())});
 Queries.Add(ValueOr(TEXT("StructValue"), TEXT("Struct"), MoveTemp(WrongStruct)));
 FValue InlineStruct = *Field(Defaults, TEXT("StructValue"));
 *Field(InlineStruct, TEXT("DefaultValue")) = ObjectValue({TEXT("Properties"), TEXT("Struct")},
  {ObjectValue({TEXT("KeyType")}, {ObjectValue({TEXT("Class"), TEXT("Properties")},
   {StringValue(UBlackboardKeyType_Object::StaticClass()->GetPathName()), ObjectValue({TEXT("BaseClass")},
    {ObjectValue({TEXT("Kind"), TEXT("Path")}, {StringValue(TEXT("ClassRef")), StringValue(AActor::StaticClass()->GetPathName())})})})}), StringValue(FBlackboardEntry::StaticStruct()->GetPathName())});
 Queries.Add(ValueOr(TEXT("StructValue"), TEXT("StructInline"), MoveTemp(InlineStruct)));
 auto I = ClassRequest(*CDO->GetClass(), BB, MoveTemp(Queries)); FResolveExistingHandleResolver Resolver;
 Package->SetDirtyFlag(false); const auto Out = Invoke(*this, I, Resolver);
 if (!TestEqual(TEXT("typed ValueOr evidence succeeds"), Out.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 const auto* Rows = Field(Out.Value, TEXT("evidence"));
 if (!TestTrue(TEXT("all ValueOr observations returned"), Rows && Rows->Elements.Num() == 18)) return false;
 for (int32 N = 0; N < 11; ++N)
 {
  TestTrue(FString(TEXT("real authored type compatibility: ")) + Names[N], Field(Rows->Elements[N], TEXT("compatible"))->BooleanValue);
  TestTrue(TEXT("actual Blackboard key exists"), Field(Rows->Elements[N], TEXT("key_exists"))->BooleanValue);
 }
 for (int32 N = 11; N < 17; ++N) TestFalse(TEXT("actual wrong type or retained metadata rejects"), Field(Rows->Elements[N], TEXT("compatible"))->BooleanValue);
 TestTrue(TEXT("real Struct type with inline-object literal retains compatibility without preview writing its members"), Field(Rows->Elements[17], TEXT("compatible"))->BooleanValue);
 TestTrue(TEXT("CDO still readable"), Snapshot(*CDO, After)); TestEqual(TEXT("all eleven original CDO leaves unchanged"), After, Before);
 TestEqual(TEXT("inline object identity retained"), static_cast<const UObject*>(CDO->Nested.Get()), NestedBefore);
 TestEqual(TEXT("inline object authored value unchanged"), CDO->Nested->AuthoredValue, InlineBefore);
 TestFalse(TEXT("independent ValueOr evaluation does not dirty the live package"), Package->IsDirty());
 FValue Unsafe = *Field(Defaults, TEXT("StructValue"));
 *Field(Unsafe, TEXT("DefaultValue")) = ObjectValue({TEXT("Class"), TEXT("Properties")},
  {StringValue(UAssetDocumentAIInstallOwnedChild::StaticClass()->GetPathName()), ObjectValue({TEXT("AuthoredValue")}, {IntegerValue(999)})});
 I.ValueArguments.Elements[1] = ArrayValue({ValueOr(TEXT("StructValue"), TEXT("Struct"), MoveTemp(Unsafe))});
 TestNotEqual(TEXT("inline-object authored wrapper cannot reach CDO/live through an isolated struct"), Invoke(*this, I, Resolver).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestEqual(TEXT("rejected authored wrapper preserves actual inline object"), CDO->Nested->AuthoredValue, InlineBefore);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBindingSubtreeTest,
 "UEAssetDoc.AIProvider.BehaviorTree.BindingInspect.SubtreeForwardAncestryAndEnumEvidence",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBindingSubtreeTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBindingInspectTests;
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/AssetDocumentAI/BindingSubtree_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
 auto* Parent = NewObject<UBlackboardData>(Package.Get(), TEXT("Parent"), RF_Public);
 auto* Child = NewObject<UBlackboardData>(Package.Get(), TEXT("Child"), RF_Public); Child->Parent = Parent;
 auto* Sibling = NewObject<UBlackboardData>(Package.Get(), TEXT("Sibling"), RF_Public); Sibling->Parent = Parent;
 auto* Tree = NewObject<UBehaviorTree>(Package.Get(), TEXT("Subtree"), RF_Public); Tree->BlackboardAsset = Parent;
 Tree->RootDecorators.Add(NewObject<UBTDecorator_Cooldown>(Tree));
 auto* EnumKey = Parent->UpdatePersistentKey<UBlackboardKeyType_Enum>(TEXT("Mode")); EnumKey->EnumType = StaticEnum<EAssetDocumentAIReadbackEnum>();
 Child->UpdateParentKeys(); Sibling->UpdateParentKeys();
 FResolveExistingHandleResolver Resolver;
 UBTTask_RunBehavior::StaticClass()->GetDefaultObject(); UBTTask_RunBehaviorDynamic::StaticClass()->GetDefaultObject();
 for (bool Static : {false, true})
 {
  UClass* Class = Static ? UBTTask_RunBehavior::StaticClass() : UBTTask_RunBehaviorDynamic::StaticClass();
  auto I = ClassRequest(*Class, Child, {Subtree(Static, Tree), Subtree(Static, nullptr)});
  const auto Out = Invoke(*this, I, Resolver);
  if (!TestEqual(TEXT("subtree actual evidence is readable"), Out.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  const auto* Rows = Field(Out.Value, TEXT("evidence"));
  if (!TestTrue(TEXT("subtree query cardinality"), Rows && Rows->Elements.Num() == 2)) return false;
  TestTrue(TEXT("effective child BB accepts ancestor subtree BB"), Field(Rows->Elements[0], TEXT("effective_blackboard_compatible"))->BooleanValue);
  TestEqual(TEXT("actual subtree Blackboard identity"), Field(Rows->Elements[0], TEXT("blackboard_path_or_null"))->StringValue, Parent->GetPathName());
  TestTrue(TEXT("root decorators are evidence for both query groups"), Field(Rows->Elements[0], TEXT("has_root_decorators"))->BooleanValue);
  TestFalse(TEXT("null subtree existence evidence"), Field(Rows->Elements[1], TEXT("exists"))->BooleanValue);
  Tree->BlackboardAsset = Child; I.ValueArguments.Elements[0] = StringValue(Parent->GetPathName());
  const auto Reverse = Invoke(*this, I, Resolver);
  if (!TestEqual(TEXT("reverse query still returns evidence"), Reverse.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  TestFalse(TEXT("subtree descendant BB does not pass reverse ancestry"), Field(Field(Reverse.Value, TEXT("evidence"))->Elements[0], TEXT("effective_blackboard_compatible"))->BooleanValue);
  I.ValueArguments.Elements[0] = StringValue(Sibling->GetPathName());
  const auto Related = Invoke(*this, I, Resolver);
  if (!TestEqual(TEXT("sibling query returns evidence"), Related.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  TestFalse(TEXT("related siblings are insufficient for forward compatibility"), Field(Field(Related.Value, TEXT("evidence"))->Elements[0], TEXT("effective_blackboard_compatible"))->BooleanValue);
  Tree->BlackboardAsset = Parent;
 }
 auto* CDO = UBTDecorator_Blackboard::StaticClass()->GetDefaultObject(); TArray<uint8> Before, After;
 if (!TestTrue(TEXT("enum decorator preimage"), Snapshot(*CDO, Before))) return false;
 auto I = ClassRequest(*CDO->GetClass(), Parent, {Selector({Member(TEXT("BlackboardKey"))}, TEXT("Mode"))});
 const auto Out = Invoke(*this, I, Resolver);
 if (!TestEqual(TEXT("real enum selector evidence"), Out.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 const auto* Values = Field(Field(Out.Value, TEXT("evidence"))->Elements[0], TEXT("enum_values_or_null"));
 if (!TestTrue(TEXT("actual enum token map exists"), Values && Values->Kind == EAssetDocumentRuntimeValueKind::Object && Field(*Values, TEXT("First")))) return false;
 TestEqual(TEXT("actual authored enum numeric value"), Field(*Values, TEXT("First"))->IntegerValue, int64(3));
 TestTrue(TEXT("actual Blackboard decorator selector identity is explicit"), Field(Field(Out.Value, TEXT("evidence"))->Elements[0], TEXT("blackboard_decorator_selector"))->BooleanValue);
 TestNull(TEXT("unknown enum token not synthesized"), Field(*Values, TEXT("Missing")));
 auto* Unqualified = NewObject<UEnum>(Package.Get(), TEXT("UnqualifiedEnumClass"), RF_Public);
 TArray<TPair<FName, int64>> UnqualifiedNames{{FName(TEXT("SharedValue")), 1}, {FName(TEXT("OtherValue")), 2}};
 TestTrue(TEXT("stock unqualified enum-class fixture is initialized"), AssetDocumentEnums::SetEnums(*Unqualified, UnqualifiedNames, UEnum::ECppForm::EnumClass, EEnumFlags::None, false));
 TestTrue(TEXT("stock short alias is absent for an unqualified enum-class entry"), Unqualified->GetNameStringByIndex(0).IsEmpty());
 EnumKey->EnumType = Unqualified;
 const auto UnqualifiedOut = Invoke(*this, I, Resolver);
 if (!TestEqual(TEXT("full enum token remains readable when its short alias is absent"), UnqualifiedOut.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 const auto* UnqualifiedValues = Field(Field(UnqualifiedOut.Value, TEXT("evidence"))->Elements[0], TEXT("enum_values_or_null"));
 if (!TestTrue(TEXT("unqualified full enum token is retained"), UnqualifiedValues && Field(*UnqualifiedValues, TEXT("SharedValue")))) return false;
 TestEqual(TEXT("unqualified enum token has its actual integer"), Field(*UnqualifiedValues, TEXT("SharedValue"))->IntegerValue, int64(1));
 TestTrue(TEXT("enum decorator remains readable"), Snapshot(*CDO, After)); TestEqual(TEXT("enum evidence does not modify decorator CDO"), After, Before);
 Package->SetDirtyFlag(false);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBindingLiveTest,
 "UEAssetDoc.AIProvider.BehaviorTree.BindingInspect.LiveReadOnlyLeaseAndNoFallback",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBindingLiveTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBindingInspectTests;
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/AssetDocumentAI/BindingLive_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
 TStrongObjectPtr<UBehaviorTree> Tree(NewObject<UBehaviorTree>(Package.Get(), TEXT("Tree"), RF_Public | RF_Standalone));
 auto* BB = NewObject<UBlackboardData>(Package.Get(), TEXT("BB"), RF_Public);
 BB->UpdatePersistentKey<UBlackboardKeyType_Bool>(TEXT("Bool")); Tree->BlackboardAsset = BB;
 BB->UpdatePersistentKey<UBlackboardKeyType_Int>(TEXT("Int32"));
 BB->UpdatePersistentKey<UBlackboardKeyType_Class>(TEXT("Actor"))->BaseClass = AActor::StaticClass();
 BB->UpdatePersistentKey<UBlackboardKeyType_Class>(TEXT("Broad"))->BaseClass = UObject::StaticClass();
 FScopedTransaction Fixture(FText::FromString(TEXT("Read-only BT binding fixture")));
 auto* Graph = CastChecked<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(Tree.Get(), TEXT("Graph"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass()));
 Graph->GraphGuid = FGuid::NewGuid(); Tree->BTGraph = Graph; Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph); Graph->OnCreated(); Graph->Initialize();
 auto* Root = CastChecked<UBehaviorTreeGraphNode_Root>(Graph->Nodes[0]); Root->BlackboardAsset = BB; Root->UpdateBlackboard();
 FAISchemaAction_NewNode CompositeAction; CompositeAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Composite>();
 CompositeAction.NodeTemplate->NodeInstance = NewObject<UBTComposite_Sequence>(CompositeAction.NodeTemplate);
 auto* Composite = CompositeAction.PerformAction(Graph, Output(*Root), FVector2f(0, 100), false);
 auto AddTask = [&](float X)
 {
  FAISchemaAction_NewNode TaskAction; TaskAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Task>();
  TaskAction.NodeTemplate->NodeInstance = NewObject<UAssetDocumentAIValueOrTask>(TaskAction.NodeTemplate);
  return CastChecked<UBehaviorTreeGraphNode_Task>(TaskAction.PerformAction(Graph, Output(*Composite), FVector2f(X, 200), false));
 };
 auto* TaskNode = AddTask(0);
 auto* Task = CastChecked<UAssetDocumentAIValueOrTask>(TaskNode->NodeInstance); Task->BoolValue.SetKey(TEXT("Bool"));
 auto* DirectNode = AddTask(200);
 auto* DirectTask = CastChecked<UAssetDocumentAIValueOrTask>(DirectNode->NodeInstance);
 auto& DirectValue = DirectTask->Containers.InitializeAs<FValueOrBBKey_Int32>(137);
 DirectValue.SetKey(TEXT("Int32"));
 auto* MultiNode = AddTask(400);
 auto* MultiTask = CastChecked<UAssetDocumentAIValueOrTask>(MultiNode->NodeInstance);
 auto& Multi = MultiTask->Containers.InitializeAs<FAssetDocumentAIValueOrContainers>();
 Multi.Nested.Identity = 73; Multi.Nested.Value.SetKey(TEXT("Int32"));
 Multi.Array.AddDefaulted(); Multi.Array[0].Value.SetKey(TEXT("Int32"));
 auto& NestedClass = Multi.Dynamic.InitializeAs<FValueOrBBKey_Class>();
 NestedClass.SetBaseClass(AActor::StaticClass()); NestedClass.SetKey(TEXT("Actor"));
 Fixture.Cancel(); Package->SetDirtyFlag(false); TArray<uint8> Before, DirectBefore, MultiBefore, After;
 if (!TestTrue(TEXT("live node preimage"), Snapshot(*Task, Before))
  || !TestTrue(TEXT("direct dynamic live node preimage"), Snapshot(*DirectTask, DirectBefore))
  || !TestTrue(TEXT("multi-field dynamic live node preimage"), Snapshot(*MultiTask, MultiBefore))) return false;
 FResolveExistingHandleResolver Resolver;
 auto I = ClassRequest(*Task->GetClass(), BB, {ValueOr(TEXT("BoolValue"), TEXT("Bool"))});
 FAssetDocumentMutationToken Token; Token.RequestId = I.RequestId; Token.TransactionId = I.TransactionId; Token.RuntimeRevision = I.RuntimeRevision; Resolver.SetMutationToken(Token);
 auto Resolve = I; Resolve.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); Resolve.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
 Resolve.ValueArguments = ObjectValue({TEXT("expected_class"), TEXT("path")}, {StringValue(UBehaviorTree::StaticClass()->GetPathName()), StringValue(Tree->GetPathName())});
 const auto Resolved = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Resolve, Resolver, nullptr);
 if (!TestEqual(TEXT("real read-only asset resolution succeeds"), Resolved.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 TOptional<FAssetDocumentOpaqueHandle> Read;
 for (const auto& Result : Resolved.HandleResults)
  if (Result.Name == TEXT("asset_read") && Result.Handles.Num() == 1) Read = Resolver.RegisterProviderScopedAlias(Result.Handles[0], TEXTVIEW("ai"), EAssetDocumentHandleAccess::ReadOnly);
 if (!TestTrue(TEXT("real AI read-only asset alias"), Read.IsSet())) return false;
 I.HandleArguments = {{TEXT("asset"), Read.GetValue()}};
 I.ValueArguments.Elements[2] = ObjectValue({TEXT("behavior_tree_path"), TEXT("kind"), TEXT("stable_node_id")},
  {StringValue(Tree->GetPathName()), StringValue(TEXT("live_node")), StringValue(TaskNode->NodeGuid.ToString(EGuidFormats::Digits))});
 const auto Out = Invoke(*this, I, Resolver);
 if (!TestEqual(TEXT("Validate reads exact live leaf without a transaction"), Out.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 TestTrue(TEXT("retained Bool matches real Blackboard type"), Field(Field(Out.Value, TEXT("evidence"))->Elements[0], TEXT("compatible"))->BooleanValue);
 TestTrue(TEXT("live node readable afterward"), Snapshot(*Task, After)); TestEqual(TEXT("live node byte projection unchanged"), After, Before);
 TestFalse(TEXT("read-only query leaves live package clean"), Package->IsDirty()); TestEqual(TEXT("read-only query does not invalidate handles"), Resolver.GetInvalidationCount(), 0);
 auto Unchanged = [&](const TCHAR* Case)
 {
  auto NodeUnchanged = [&](UObject& Node, const TArray<uint8>& Expected, const TCHAR* Label)
  {
   const FString Prefix = FString(Case) + TEXT(": ") + Label;
   TestTrue(Prefix + TEXT(" snapshot readable"), Snapshot(Node, After));
   if (After != Expected) AddInfo(FString::Printf(TEXT("%s: before bytes=%d after bytes=%d"), *Prefix, Expected.Num(), After.Num()));
   TestEqual(Prefix + TEXT(" exact live bytes unchanged"), After, Expected);
  };
  NodeUnchanged(*Task, Before, TEXT("original live node"));
  NodeUnchanged(*DirectTask, DirectBefore, TEXT("direct dynamic live node"));
  NodeUnchanged(*MultiTask, MultiBefore, TEXT("multi-field dynamic live node"));
  TestFalse(FString(Case) + TEXT(": live package remains clean"), Package->IsDirty());
  TestEqual(FString(Case) + TEXT(": no handle invalidation"), Resolver.GetInvalidationCount(), 0);
 };
 Unchanged(TEXT("retained Bool"));
 auto DynamicStep = [](const UScriptStruct& Type)
 { return ObjectValue({TEXT("kind"), TEXT("struct_path")}, {StringValue(TEXT("instanced_struct")), StringValue(Type.GetPathName())}); };
 auto RetainedAt = [](TArray<FValue> Locator, const TCHAR* Key)
 { return ObjectValue({TEXT("authored_value_or_null"), TEXT("kind"), TEXT("locator"), TEXT("selected_key")},
   {NullValue(), StringValue(TEXT("value_or")), ArrayValue(MoveTemp(Locator)), StringValue(Key)}); };
 const TArray<FValue> DirectLocator = {Member(TEXT("Containers")), DynamicStep(*FValueOrBBKey_Int32::StaticStruct())};
 auto DynamicI = I;
 DynamicI.ValueArguments.Elements[2].Elements[2] = StringValue(DirectNode->NodeGuid.ToString(EGuidFormats::Digits));
 for (const TCHAR* Key : {TEXT("Int32"), TEXT("Missing"), TEXT("Bool")})
 {
  DynamicI.ValueArguments.Elements[1] = ArrayValue({RetainedAt(DirectLocator, Key)});
  const auto DynamicOut = Invoke(*this, DynamicI, Resolver);
  const bool Success = TestEqual(TEXT("retained direct dynamic ValueOr returns real evidence"), DynamicOut.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  Unchanged(*FString::Printf(TEXT("direct dynamic key=%s"), Key));
  if (!Success) return false;
  const auto* Evidence = Field(DynamicOut.Value, TEXT("evidence"));
  if (!TestTrue(TEXT("direct dynamic evidence cardinality"), Evidence && Evidence->Elements.Num() == 1)) return false;
  TestEqual(TEXT("direct dynamic key existence is independent of compatibility"), Field(Evidence->Elements[0], TEXT("key_exists"))->BooleanValue, FString(Key) != TEXT("Missing"));
  TestEqual(TEXT("direct dynamic actual Int32 accepts only the matching key type"), Field(Evidence->Elements[0], TEXT("compatible"))->BooleanValue, FString(Key) == TEXT("Int32"));
  TestTrue(TEXT("direct dynamic query returns no mutation or handles"), DynamicOut.ChangedSet.IsEmpty() && DynamicOut.HandleResults.IsEmpty());
 }
 DynamicI.ValueArguments.Elements[2].Elements[2] = StringValue(MultiNode->NodeGuid.ToString(EGuidFormats::Digits));
 const auto OuterDynamic = DynamicStep(*FAssetDocumentAIValueOrContainers::StaticStruct());
 const TArray<FValue> ClassLocator = {Member(TEXT("Containers")), OuterDynamic, Member(TEXT("Dynamic")), DynamicStep(*FValueOrBBKey_Class::StaticStruct())};
 const TArray<FValue> MultiQueries = {
  RetainedAt({Member(TEXT("Containers")), OuterDynamic, Member(TEXT("Nested")), Member(TEXT("Value"))}, TEXT("Int32")),
  RetainedAt({Member(TEXT("Containers")), OuterDynamic, Member(TEXT("Array")),
   ObjectValue({TEXT("index"), TEXT("kind")}, {IntegerValue(0), StringValue(TEXT("array_element"))}), Member(TEXT("Value"))}, TEXT("Int32")),
  RetainedAt(ClassLocator, TEXT("Actor")), RetainedAt(ClassLocator, TEXT("Broad"))};
 for (int32 Index = 0; Index < MultiQueries.Num(); ++Index)
 {
  DynamicI.ValueArguments.Elements[1] = ArrayValue({MultiQueries[Index]});
  const auto DynamicOut = Invoke(*this, DynamicI, Resolver);
  const bool Success = TestEqual(TEXT("multi-field live dynamic wrapper preserves exact typed descendants"), DynamicOut.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  Unchanged(*FString::Printf(TEXT("multi-field dynamic query=%d"), Index));
  if (!Success) return false;
  const auto* Evidence = Field(DynamicOut.Value, TEXT("evidence"));
  if (!TestTrue(TEXT("multi-field dynamic evidence cardinality"), Evidence && Evidence->Elements.Num() == 1)) return false;
  TestTrue(TEXT("all multi-field query keys actually exist"), Field(Evidence->Elements[0], TEXT("key_exists"))->BooleanValue);
  TestEqual(TEXT("retained nested Class reads live Actor BaseClass rather than broad defaults"), Field(Evidence->Elements[0], TEXT("compatible"))->BooleanValue, Index != 3);
  TestTrue(TEXT("multi-field dynamic query returns no mutation or handles"), DynamicOut.ChangedSet.IsEmpty() && DynamicOut.HandleResults.IsEmpty());
 }
 auto WrongDynamic = I;
 WrongDynamic.ValueArguments.Elements[2].Elements[2] = StringValue(DirectNode->NodeGuid.ToString(EGuidFormats::Digits));
 WrongDynamic.ValueArguments.Elements[1] = ArrayValue({RetainedAt({Member(TEXT("Containers")), DynamicStep(*FValueOrBBKey_Bool::StaticStruct())}, TEXT("Bool"))});
 const auto Mismatch = Invoke(*this, WrongDynamic, Resolver);
 TestNotEqual(TEXT("live dynamic type mismatch cannot use a declared default"), Mismatch.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestTrue(TEXT("dynamic mismatch is an exact locator rejection"), Mismatch.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("AIProvider.BehaviorTree.BindingLocatorRejected"); }));
 Unchanged(TEXT("direct dynamic type mismatch"));
 WrongDynamic.ValueArguments.Elements[1] = ArrayValue({RetainedAt({Member(TEXT("Containers")), DynamicStep(*FValueOrBBKey_Int32::StaticStruct()), Member(TEXT("NonAuthored"))}, TEXT("Int32"))});
 const auto UnknownField = Invoke(*this, WrongDynamic, Resolver);
 TestNotEqual(TEXT("live dynamic leaf cannot expose a nonexistent authored child"), UnknownField.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestTrue(TEXT("unknown dynamic child is an exact locator rejection"), UnknownField.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("AIProvider.BehaviorTree.BindingLocatorRejected"); }));
 Unchanged(TEXT("direct dynamic unknown field"));
 auto Wrong = I; Wrong.ValueArguments.Elements[2].Elements[2] = StringValue(FGuid::NewGuid().ToString(EGuidFormats::Digits));
 TestNotEqual(TEXT("unavailable live stable ID cannot use CDO fallback"), Invoke(*this, Wrong, Resolver).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 Unchanged(TEXT("missing stable ID"));
 Wrong = I; Wrong.HandleArguments[0].Handle.Generation += 1;
 TestNotEqual(TEXT("stale readonly lease generation rejected"), Invoke(*this, Wrong, Resolver).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 Unchanged(TEXT("stale handle generation"));
 Wrong = I; Wrong.ValueArguments.Elements[1] = ArrayValue({ObjectValue({TEXT("authored_value_or_null"), TEXT("kind"), TEXT("locator"), TEXT("selected_key")},
  {NullValue(), StringValue(TEXT("value_or")), ArrayValue({Member(TEXT("Containers")),
   ObjectValue({TEXT("kind"), TEXT("struct_path")}, {StringValue(TEXT("instanced_struct")), StringValue(FAssetDocumentAIValueOrContainers::StaticStruct()->GetPathName())}),
   Member(TEXT("Array")), ObjectValue({TEXT("index"), TEXT("kind")}, {IntegerValue(0), StringValue(TEXT("array_element"))}), Member(TEXT("Value"))}), StringValue(TEXT("Bool"))})});
 TestNotEqual(TEXT("missing live dynamic/container element cannot create a default preview"), Invoke(*this, Wrong, Resolver).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 Unchanged(TEXT("missing dynamic/container locator"));
 TestTrue(TEXT("negative cases still preserve live projection"), Snapshot(*Task, After)); TestEqual(TEXT("failed query leaves live bytes unchanged"), After, Before);
 Tree->ClearFlags(RF_Standalone); Package->SetDirtyFlag(false);
 return !HasAnyErrors();
}
#endif
