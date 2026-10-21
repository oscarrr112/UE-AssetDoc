// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAIReadbackTestTypes.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeValueOrKeyInventory.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

namespace AssetDocumentAIAuthoredPropertyReadbackTests
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
using UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::BuildAuthoredProperties;

const FValue& Member(FAutomationTestBase& Test, const FValue& Object, const TCHAR* Key)
{
	const int32 Index = Object.ObjectKeys.IndexOfByKey(FString(Key));
	if (!Test.TestTrue(FString::Printf(TEXT("Object contains %s"), Key),
		Object.Kind == EKind::Object && Object.Elements.IsValidIndex(Index)))
	{
		static const FValue Missing;
		return Missing;
	}
	return Object.Elements[Index];
}

void Keys(FAutomationTestBase& Test, const FValue& Object, TArray<FString> Expected)
{
	Expected.Sort();
	Test.TestTrue(TEXT("Exact authored keys, no cache or uneditable fields"),
		Object.Kind == EKind::Object && Object.ObjectKeys == Expected
		&& Object.Elements.Num() == Expected.Num());
}

void CheckVector(FAutomationTestBase& Test, const FValue& Value)
{
	Keys(Test, Value, {TEXT("X"), TEXT("Y"), TEXT("Z")});
	Test.TestEqual(TEXT("Vector X"), Member(Test, Value, TEXT("X")).NumberValue, 1.25);
	Test.TestEqual(TEXT("Vector Y"), Member(Test, Value, TEXT("Y")).NumberValue, -2.5);
	Test.TestEqual(TEXT("Vector Z"), Member(Test, Value, TEXT("Z")).NumberValue, 3.75);
}

void CheckFields(FAutomationTestBase& Test, const FValue& Value)
{
	Keys(Test, Value, {TEXT("Count"), TEXT("Label")});
	Test.TestEqual(TEXT("Authored count"), Member(Test, Value, TEXT("Count")).IntegerValue, int64(37));
	Test.TestEqual(TEXT("Authored label"), Member(Test, Value, TEXT("Label")).StringValue, FString(TEXT("literal")));
}

void CheckReference(FAutomationTestBase& Test, const FValue& Value, const TCHAR* Kind, const FString& Path)
{
	Keys(Test, Value, {TEXT("Kind"), TEXT("Path")});
	Test.TestEqual(TEXT("Reference kind"), Member(Test, Value, TEXT("Kind")).StringValue, FString(Kind));
	Test.TestEqual(TEXT("Reference path"), Member(Test, Value, TEXT("Path")).StringValue, Path);
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FAssetDocumentAIAuthoredDynamicPropertyTest,
	"UEAssetDoc.AI.BehaviorTree.AuthoredProperties.InstancedStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FAssetDocumentAIAuthoredDynamicPropertyTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	Names = {TEXT("Null"), TEXT("EmptyEditableSurface"), TEXT("OrdinaryFields"), TEXT("CustomizedVector")};
	Commands = Names;
}

bool FAssetDocumentAIAuthoredDynamicPropertyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAIAuthoredPropertyReadbackTests;
	TStrongObjectPtr<UAssetDocumentAIReadbackDynamicFixture> Fixture(NewObject<UAssetDocumentAIReadbackDynamicFixture>());
	UScriptStruct* ExpectedType = nullptr;
	if (Parameters == TEXT("EmptyEditableSurface"))
	{
		Fixture->Dynamic.InitializeAs<FAssetDocumentAIReadbackEmpty>();
		ExpectedType = FAssetDocumentAIReadbackEmpty::StaticStruct();
	}
	else if (Parameters == TEXT("OrdinaryFields"))
	{
		Fixture->Dynamic.InitializeAs<FAssetDocumentAIReadbackFields>();
		ExpectedType = FAssetDocumentAIReadbackFields::StaticStruct();
	}
	else if (Parameters == TEXT("CustomizedVector"))
	{
		Fixture->Dynamic.InitializeAs<FVector>(FVector(1.25, -2.5, 3.75));
		ExpectedType = TBaseStructure<FVector>::Get();
	}
	FValue Result;
	if (!TestTrue(TEXT("Normal Editor row generator projects dynamic property"), BuildAuthoredProperties(*Fixture, Result))) return false;
	Keys(*this, Result, {TEXT("Dynamic")});
	const FValue& Dynamic = Member(*this, Result, TEXT("Dynamic"));
	if (!ExpectedType)
	{
		TestTrue(TEXT("Null instance remains null"), Dynamic.Kind == EKind::Null);
		return true;
	}
	Keys(*this, Dynamic, {TEXT("Properties"), TEXT("Struct")});
	TestEqual(TEXT("Exact dynamic script struct"), Member(*this, Dynamic, TEXT("Struct")).StringValue, ExpectedType->GetPathName());
	const FValue& Properties = Member(*this, Dynamic, TEXT("Properties"));
	if (Parameters == TEXT("EmptyEditableSurface")) Keys(*this, Properties, {});
	else if (Parameters == TEXT("OrdinaryFields")) CheckFields(*this, Properties);
	else CheckVector(*this, Properties);
	TestTrue(TEXT("Readback leaves dynamic type intact"), Fixture->Dynamic.GetScriptStruct() == ExpectedType);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIAuthoredValuesPropertyTest,
	"UEAssetDoc.AI.BehaviorTree.AuthoredProperties.StructSelectorContainersAndSoftReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIAuthoredValuesPropertyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAIAuthoredPropertyReadbackTests;
	TStrongObjectPtr<UAssetDocumentAIReadbackValuesFixture> Fixture(NewObject<UAssetDocumentAIReadbackValuesFixture>());
	Fixture->Selector.SelectedKeyName = TEXT("Target");
	const FString ObjectPath = TEXT("/Game/AssetDocumentReadbackMissing/Unloaded.Unloaded");
	const FString ClassPath = TEXT("/Game/AssetDocumentReadbackMissing/UnloadedClass.UnloadedClass_C");
	Fixture->SoftObject = TSoftObjectPtr<UObject>(FSoftObjectPath(ObjectPath));
	Fixture->SoftClass = TSoftClassPtr<UObject>(FSoftObjectPath(ClassPath));
	Fixture->Array = {9, 2, 9};
	Fixture->StringMap.Add(TEXT("Zulu"), 9); Fixture->StringMap.Add(TEXT("Alpha"), 2);
	Fixture->NameMap.Add(TEXT("Zulu"), 9); Fixture->NameMap.Add(TEXT("Alpha"), 2);
	Fixture->ByteMap.Add(7, 9); Fixture->ByteMap.Add(3, 2);
	Fixture->EnumMap.Add(EAssetDocumentAIReadbackEnum::Second, 9);
	Fixture->EnumMap.Add(EAssetDocumentAIReadbackEnum::First, 2);
	Fixture->Set.Add(TEXT("Zulu")); Fixture->Set.Add(TEXT("Alpha")); Fixture->Set.Add(TEXT("Beta"));
	TestTrue(TEXT("Object target initially unloaded"), Fixture->SoftObject.IsPending());
	TestTrue(TEXT("Class target initially unloaded"), Fixture->SoftClass.IsPending());
	FValue Result;
	if (!TestTrue(TEXT("Full authored surface projects"), BuildAuthoredProperties(*Fixture, Result))) return false;
	CheckFields(*this, Member(*this, Result, TEXT("Ordinary")));
	CheckVector(*this, Member(*this, Result, TEXT("Vector")));
	const FValue& Selector = Member(*this, Result, TEXT("Selector"));
	Keys(*this, Selector, {TEXT("Key")});
	TestEqual(TEXT("Selector name only"), Member(*this, Selector, TEXT("Key")).StringValue, FString(TEXT("Target")));
	CheckReference(*this, Member(*this, Result, TEXT("SoftObject")), TEXT("AssetRef"), ObjectPath);
	CheckReference(*this, Member(*this, Result, TEXT("SoftClass")), TEXT("ClassRef"), ClassPath);
	TestTrue(TEXT("Readback does not load object"), Fixture->SoftObject.IsPending());
	TestTrue(TEXT("Readback does not load class"), Fixture->SoftClass.IsPending());
	const FValue& Array = Member(*this, Result, TEXT("Array"));
	if (TestTrue(TEXT("Array preserves duplicates and size"), Array.Kind == EKind::Array && Array.Elements.Num() == 3))
	{
		TestEqual(TEXT("Array first"), Array.Elements[0].IntegerValue, int64(9));
		TestEqual(TEXT("Array second"), Array.Elements[1].IntegerValue, int64(2));
		TestEqual(TEXT("Array third"), Array.Elements[2].IntegerValue, int64(9));
	}
	for (const TCHAR* MapName : {TEXT("StringMap"), TEXT("NameMap"), TEXT("ByteMap"), TEXT("EnumMap")})
	{
		const bool bNumeric = FString(MapName) == TEXT("ByteMap") || FString(MapName) == TEXT("EnumMap");
		const FValue& Map = Member(*this, Result, MapName);
		const TCHAR* First = bNumeric ? TEXT("3") : TEXT("Alpha");
		const TCHAR* Second = bNumeric ? TEXT("7") : TEXT("Zulu");
		Keys(*this, Map, {First, Second});
		TestEqual(TEXT("Map first value"), Member(*this, Map, First).IntegerValue, int64(2));
		TestEqual(TEXT("Map second value"), Member(*this, Map, Second).IntegerValue, int64(9));
	}
	// Existing canonical-family design §4.3 encodes string kind, big-endian
	// UTF-8 length, then bytes: Beta/Zulu (4) precede Alpha (5); equal lengths
	// compare their payload. These are literal expectations, not a test codec.
	auto CheckSet = [&](const FValue& Set)
	{
		if (TestTrue(TEXT("Set has three canonical members"), Set.Kind == EKind::Array && Set.Elements.Num() == 3))
		{
			TestEqual(TEXT("Set equal-length first"), Set.Elements[0].StringValue, FString(TEXT("Beta")));
			TestEqual(TEXT("Set equal-length second"), Set.Elements[1].StringValue, FString(TEXT("Zulu")));
			TestEqual(TEXT("Set longer payload last"), Set.Elements[2].StringValue, FString(TEXT("Alpha")));
		}
	};
	CheckSet(Member(*this, Result, TEXT("Set")));
	Fixture->Set.Reset();
	Fixture->Set.Add(TEXT("Alpha")); Fixture->Set.Add(TEXT("Beta")); Fixture->Set.Add(TEXT("Zulu"));
	FValue Reordered;
	if (TestTrue(TEXT("Different native set insertion order projects"), BuildAuthoredProperties(*Fixture, Reordered)))
		CheckSet(Member(*this, Reordered, TEXT("Set")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIAuthoredFamiliesPropertyTest,
	"UEAssetDoc.AI.BehaviorTree.AuthoredProperties.AllElevenValueOrBBKeyFamilies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIAuthoredFamiliesPropertyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAIAuthoredPropertyReadbackTests;
	TStrongObjectPtr<UAssetDocumentAIReadbackFamiliesFixture> Fixture(NewObject<UAssetDocumentAIReadbackFamiliesFixture>());
	Fixture->BoolValue = FValueOrBBKey_Bool(true);
	Fixture->ClassValue = FValueOrBBKey_Class(TSubclassOf<UObject>(UObject::StaticClass()));
	Fixture->EnumValue = FValueOrBBKey_Enum(EAssetDocumentAIReadbackEnum::Second);
	Fixture->FloatValue = FValueOrBBKey_Float(1.25f);
	Fixture->IntValue = FValueOrBBKey_Int32(37);
	Fixture->NameValue = FValueOrBBKey_Name(TEXT("LiteralName"));
	Fixture->StringValue = FValueOrBBKey_String(TEXT("literal"));
	Fixture->RotatorValue = FValueOrBBKey_Rotator(FRotator(10, 20, 30));
	Fixture->StructValue = FValueOrBBKey_Struct(FAssetDocumentAIReadbackFields());
	Fixture->VectorValue = FValueOrBBKey_Vector(FVector(1.25, -2.5, 3.75));
	FValueOrBlackboardKeyBase* Families[] = {&Fixture->BoolValue, &Fixture->ClassValue,
		&Fixture->EnumValue, &Fixture->FloatValue, &Fixture->IntValue, &Fixture->NameValue,
		&Fixture->StringValue, &Fixture->ObjectValue, &Fixture->RotatorValue,
		&Fixture->StructValue, &Fixture->VectorValue};
	for (FValueOrBlackboardKeyBase* Family : Families) Family->SetKey(TEXT("BoundKey"));
	FValue Result;
	if (!TestTrue(TEXT("All eleven bound families retain authored defaults"), BuildAuthoredProperties(*Fixture, Result))) return false;
	TestEqual(TEXT("Eleven families"), Result.ObjectKeys.Num(), 11);
	for (const FString& Name : Result.ObjectKeys)
	{
		const FValue& Family = Member(*this, Result, *Name);
		TArray<FString> Expected = {TEXT("DefaultValue"), TEXT("Key")};
		if (Name == TEXT("ClassValue") || Name == TEXT("ObjectValue")) Expected.Add(TEXT("BaseClass"));
		if (Name == TEXT("EnumValue")) { Expected.Add(TEXT("EnumType")); Expected.Add(TEXT("NativeEnumTypeName")); }
		if (Name == TEXT("StructValue")) Expected.Add(TEXT("bCanEditDefaultValueType"));
		Keys(*this, Family, MoveTemp(Expected));
		TestEqual(TEXT("Bound key retained with default"), Member(*this, Family, TEXT("Key")).StringValue, FString(TEXT("BoundKey")));
	}
	auto Default = [&](const TCHAR* Name) -> const FValue& { return Member(*this, Member(*this, Result, Name), TEXT("DefaultValue")); };
	TestTrue(TEXT("Bool literal"), Default(TEXT("BoolValue")).Kind == EKind::Boolean && Default(TEXT("BoolValue")).BooleanValue);
	TestEqual(TEXT("Enum literal"), Default(TEXT("EnumValue")).IntegerValue, int64(7));
	TestEqual(TEXT("Float literal"), Default(TEXT("FloatValue")).NumberValue, 1.25);
	TestEqual(TEXT("Int literal"), Default(TEXT("IntValue")).IntegerValue, int64(37));
	TestEqual(TEXT("Name literal"), Default(TEXT("NameValue")).StringValue, FString(TEXT("LiteralName")));
	TestEqual(TEXT("String literal"), Default(TEXT("StringValue")).StringValue, FString(TEXT("literal")));
	TestTrue(TEXT("Object null default"), Default(TEXT("ObjectValue")).Kind == EKind::Null);
	CheckReference(*this, Default(TEXT("ClassValue")), TEXT("ClassRef"), UObject::StaticClass()->GetPathName());
	for (const TCHAR* Name : {TEXT("ClassValue"), TEXT("ObjectValue")})
		CheckReference(*this, Member(*this, Member(*this, Result, Name), TEXT("BaseClass")), TEXT("ClassRef"), UObject::StaticClass()->GetPathName());
	const FValue& Enum = Member(*this, Result, TEXT("EnumValue"));
	CheckReference(*this, Member(*this, Enum, TEXT("EnumType")), TEXT("AssetRef"), StaticEnum<EAssetDocumentAIReadbackEnum>()->GetPathName());
	TestEqual(TEXT("Enum schema name"), Member(*this, Enum, TEXT("NativeEnumTypeName")).StringValue, StaticEnum<EAssetDocumentAIReadbackEnum>()->GetName());
	const FValue& Rotator = Default(TEXT("RotatorValue"));
	Keys(*this, Rotator, {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")});
	TestEqual(TEXT("Pitch literal"), Member(*this, Rotator, TEXT("Pitch")).NumberValue, 10.0);
	TestEqual(TEXT("Yaw literal"), Member(*this, Rotator, TEXT("Yaw")).NumberValue, 20.0);
	TestEqual(TEXT("Roll literal"), Member(*this, Rotator, TEXT("Roll")).NumberValue, 30.0);
	CheckVector(*this, Default(TEXT("VectorValue")));
	const FValue& Struct = Default(TEXT("StructValue"));
	Keys(*this, Struct, {TEXT("Properties"), TEXT("Struct")});
	TestEqual(TEXT("Family dynamic type"), Member(*this, Struct, TEXT("Struct")).StringValue, FAssetDocumentAIReadbackFields::StaticStruct()->GetPathName());
	CheckFields(*this, Member(*this, Struct, TEXT("Properties")));
	const FValue& CanEdit = Member(*this, Member(*this, Result, TEXT("StructValue")), TEXT("bCanEditDefaultValueType"));
	TestTrue(TEXT("Schema authored edit flag"), CanEdit.Kind == EKind::Boolean && !CanEdit.BooleanValue);
	return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FAssetDocumentAIAuthoredRejectPropertyTest,
	"UEAssetDoc.AI.BehaviorTree.AuthoredProperties.RejectsWholeProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FAssetDocumentAIAuthoredRejectPropertyTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	Names = {TEXT("UnsupportedMapKey"), TEXT("UnsupportedWeakReference"), TEXT("UnsignedOverflow"), TEXT("NodeBudgetOverflow")};
	Commands = Names;
}

bool FAssetDocumentAIAuthoredRejectPropertyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAIAuthoredPropertyReadbackTests;
	TStrongObjectPtr<UObject> Fixture;
	if (Parameters == TEXT("UnsupportedMapKey"))
	{
		auto* Object = NewObject<UAssetDocumentAIReadbackUnsupportedMapFixture>();
		Object->Unsupported.Add(1, 2); Fixture.Reset(Object);
	}
	else if (Parameters == TEXT("UnsupportedWeakReference")) Fixture.Reset(NewObject<UAssetDocumentAIReadbackUnsupportedLeafFixture>());
	else if (Parameters == TEXT("UnsignedOverflow")) Fixture.Reset(NewObject<UAssetDocumentAIReadbackOverflowFixture>());
	else
	{
		auto* Object = NewObject<UAssetDocumentAIReadbackValuesFixture>();
		Object->Array.Init(7, 4097); Fixture.Reset(Object);
	}
	FValue Result;
	Result.Kind = EKind::String; Result.StringValue = TEXT("stale caller output");
	TestFalse(TEXT("Unrepresentable authored property rejects all output"), BuildAuthoredProperties(*Fixture, Result));
	TestTrue(TEXT("Failure clears stale output"), Result.Kind == FValue().Kind && Result.StringValue.IsEmpty()
		&& Result.ObjectKeys.IsEmpty() && Result.Elements.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIAuthoredCodecGoldenTest,
 "UEAssetDoc.AI.BehaviorTree.AuthoredProperties.CanonicalCodec.Golden",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIAuthoredCodecGoldenTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIAuthoredPropertyReadbackTests;
 using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
 using UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::HashEvidence;
 struct FGoldenVector { const TCHAR* Name; FValue Value; const TCHAR* Hex; const TCHAR* Hash; };
 // Fixed tags/lengths/payloads were hand-written from the frozen byte contract.
 // Hashes were computed independently with Python hashlib.sha256(hex_ascii).
 // No expected value is produced by the encoder under test.
 const TArray<FGoldenVector> Vectors = {
  {TEXT("EmptyArray"), ArrayValue({}),
   TEXT("060000000000000000"),
   TEXT("798df66fa5290dde3eb90b4d8c9610aceb09fe084e16fceea49767bd0acbe6a3")},
  {TEXT("EmptyObject"), ObjectValue({}, {}),
   TEXT("070000000000000000"),
   TEXT("55df1adca31e98f1228a4c7395272d599c59dd5e1062c831aa982d2d5d793c81")},
  {TEXT("Scalars"), ArrayValue({NullValue(), BooleanValue(true), BooleanValue(false), IntegerValue(-2), NumberValue(1.5)}),
   TEXT("060000000000000005000101010002fffffffffffffffe033ff8000000000000"),
   TEXT("c21eb7f8e442cf6abbf1fc496f904723f996905700f6882bb0ce5c6dc910bb77")},
  {TEXT("UnicodeName"), ArrayValue({StringValue(TEXT("钥匙_Å")), StringValue(FName(TEXT("DisplayKey_3")).ToString())}),
   TEXT("060000000000000002040000000000000009e992a5e58c995fc38504000000000000000c446973706c61794b65795f33"),
   TEXT("7a5b84f8a6498956c5b12f7720132f42982176e5c194e88e2c69d00a352f26fb")},
  {TEXT("PositiveZero"), NumberValue(0.0),
   TEXT("030000000000000000"),
   TEXT("e996892f90836a6ef8fc486999c46dd78742aba33c9f91a9dffd132528c4c652")},
  {TEXT("NegativeZero"), NumberValue(-0.0),
   TEXT("030000000000000000"),
   TEXT("e996892f90836a6ef8fc486999c46dd78742aba33c9f91a9dffd132528c4c652")},
  {TEXT("Envelopes"), ArrayValue({IntegerValue(7), ObjectValue({TEXT("Kind"), TEXT("Path")}, {StringValue(TEXT("AssetRef")), StringValue(TEXT("/Game/A.A"))}), ObjectValue({TEXT("Properties"), TEXT("Struct")}, {ObjectValue({TEXT("Count")}, {IntegerValue(37)}), StringValue(TEXT("/Script/Test.S"))})}),
   TEXT("0600000000000000030200000000000000070700000000000000020400000000000000044b696e640400000000000000084173736574526566040000000000000004506174680400000000000000092f47616d652f412e4107000000000000000204000000000000000a50726f70657274696573070000000000000001040000000000000005436f756e7402000000000000002504000000000000000653747275637404000000000000000e2f5363726970742f546573742e53"),
   TEXT("2b6d79a24fa4ee88f131184e65ef2a3289dbeea65ae9389e213981f0b5839066")},
  {TEXT("CanonicalMap"), ObjectValue({TEXT("Alpha"), TEXT("Zulu")}, {IntegerValue(2), IntegerValue(9)}),
   TEXT("070000000000000002040000000000000005416c7068610200000000000000020400000000000000045a756c75020000000000000009"),
   TEXT("c6e51dfd9378c2a4a64af999613ce733b0a2e3379f2dcba9f323fc693750eb50")},
  {TEXT("Array12"), ArrayValue({IntegerValue(1), IntegerValue(2)}),
   TEXT("060000000000000002020000000000000001020000000000000002"),
   TEXT("fdfa696c4862d0b4d7216a2f48afa667d133d561f2b16ef141d6354735d7779c")},
  {TEXT("Array21"), ArrayValue({IntegerValue(2), IntegerValue(1)}),
   TEXT("060000000000000002020000000000000002020000000000000001"),
   TEXT("6bf7a5b70ce91b1924d32ab1ecfa2e2d8f4d63929ab20b2ece75eddf505a0f9d")},
 };
 for (const auto& Vector : Vectors)
 {
  TArray<uint8> Bytes;
  if (!TestTrue(FString::Printf(TEXT("%s encodes"), Vector.Name), EncodeAuthoredCanonicalValue(Vector.Value, Bytes))) continue;
  FString Hex;
  for (uint8 Byte : Bytes) Hex += FString::Printf(TEXT("%02x"), Byte);
  TestEqual(FString::Printf(TEXT("%s frozen bytes"), Vector.Name), Hex, FString(Vector.Hex));
  TestEqual(FString::Printf(TEXT("%s independent SHA256"), Vector.Name), HashEvidence(Hex), FString(Vector.Hash));
 }
 // This is exactly D2's absent-graph Value shape. The wire producer consumes
 // only that public Value and the request path; no reflection or typed schema.
 const FValue PublicReadbackValue = ObjectValue(
  {TEXT("comments"), TEXT("graph_guid"), TEXT("graph_state"), TEXT("root")},
  {ArrayValue({}), NullValue(), StringValue(TEXT("absent")), NullValue()});
 FString Digest;
 TestTrue(TEXT("Public D2 Value produces the wire digest"),
  UE::AssetDocumentAI::BehaviorTreeValueOrKey::HashAuthoredGraphValue(TEXT("/Game/BT.BT"), PublicReadbackValue, Digest));
 TestEqual(TEXT("Public producer independent frozen SHA256"), Digest,
  FString(TEXT("4397177aaaebfdc849f6bb5bef2475ba69a27b3c4ed5331208d8cac0fc8642f5")));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIAuthoredCodecRejectTest,
 "UEAssetDoc.AI.BehaviorTree.AuthoredProperties.CanonicalCodec.Rejects",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIAuthoredCodecRejectTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIAuthoredPropertyReadbackTests;
 using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
 auto Reject = [&](const TCHAR* Name, const FValue& Value, int64 Limit = MAX_int32)
 {
  TArray<uint8> Bytes;
  TestFalse(Name, EncodeAuthoredCanonicalValue(Value, Bytes, Limit));
 };
 Reject(TEXT("duplicate object keys are not canonical"), ObjectValue({TEXT("A"), TEXT("A")}, {NullValue(), NullValue()}));
 Reject(TEXT("unsorted map DTO cannot change frozen byte ordering"), ObjectValue({TEXT("Zulu"), TEXT("Alpha")}, {IntegerValue(9), IntegerValue(2)}));
 Reject(TEXT("mismatched key cardinality is rejected"), ObjectValue({TEXT("A")}, {}));
 Reject(TEXT("non-finite numbers are rejected"), NumberValue(std::numeric_limits<double>::infinity()));
 Reject(TEXT("NaN is rejected"), NumberValue(std::numeric_limits<double>::quiet_NaN()));
 FValue BytesValue; BytesValue.Kind = EKind::Bytes;
 Reject(TEXT("Bytes is outside the authored codec"), BytesValue);
 Reject(TEXT("zero byte budget rejects even null"), NullValue(), 0);
 Reject(TEXT("integer cannot exceed byte budget"), IntegerValue(1), 8);
 Reject(TEXT("larger hidden byte budget is not admitted"), NullValue(), int64(MAX_int32) + 1);
 TArray<uint8> ExactBytes;
 TestTrue(TEXT("integer fits exact nine-byte budget"), EncodeAuthoredCanonicalValue(IntegerValue(1), ExactBytes, 9));
 TestEqual(TEXT("exact budget emits nine bytes"), ExactBytes.Num(), 9);
 TArray<FValue> TooMany; TooMany.Init(NullValue(), 4097);
 Reject(TEXT("public aggregate node budget remains bounded"), ArrayValue(MoveTemp(TooMany)));
 FValue TooDeep = NullValue();
 for (int32 Depth = 0; Depth < 66; ++Depth) TooDeep = ArrayValue({MoveTemp(TooDeep)});
 Reject(TEXT("public depth budget remains bounded"), TooDeep);
 return true;
}
#endif
