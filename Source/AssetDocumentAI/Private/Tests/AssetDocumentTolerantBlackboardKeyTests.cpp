// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made Blackboards whose keys hold values AssetDoc refuses as new input: Enum keys with no EnumType, a short
// Enum Name or a default that is no enumerator; Object and Class keys whose default does not satisfy BaseClass, or
// with no BaseClass; a project key type with array and struct properties; Struct keys whose value has a localized
// text or a hidden enumerator. Each reads with its warning (or marker), its own extract re-applies as a no-op, adding
// another key keeps it, and a new or changed value is still refused.

#include "AssetDocumentTolerantTestSupport.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/BlackboardData.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Internationalization/Text.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantBlackboardKeyTestsLocal
{
const TCHAR* const ProjectKeyTypeClass = TEXT("/Script/AssetDocumentTests.AssetDocumentAITypedPreimageTestKeyType");
const TCHAR* const LabelledStruct = TEXT("/Script/AssetDocumentTests.AssetDocumentAIBlackboardCanonicalStructAdmitted");

template <typename T>
T* AddBoardKey(UBlackboardData& Board, const TCHAR* Name, UClass* KeyClass = T::StaticClass())
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	T* KeyType = NewObject<T>(&Board, KeyClass, NAME_None, RF_Transactional);
	Entry.KeyType = KeyType;
	Board.Keys.Add(Entry);
	return KeyType;
}

// An edit elsewhere in the document: a new Bool key at the end.
void AppendBoolKey(const TSharedPtr<FJsonObject>& Document)
{
	TArray<TSharedPtr<FJsonValue>> Keys = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Keys"));
	Keys.Add(MakeShared<FJsonValueObject>(AssetDocumentTolerantTestSupport::Parse(TEXT(
		"{\"Category\":\"\",\"Description\":\"\",\"KeyTypeClass\":{\"Kind\":\"ClassRef\","
		"\"Path\":\"/Script/AIModule.BlackboardKeyType_Bool\"},\"KeyTypeProperties\":{\"bDefaultValue\":true},"
		"\"Name\":\"Alerted\",\"bInstanceSynced\":false}"))));
	Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Keys"), Keys);
}

TSharedPtr<FJsonObject> KeyTypePropertiesAt(const TSharedPtr<FJsonObject>& Document, const int32 Index)
{
	return Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Keys"))[Index]->AsObject()
		->GetObjectField(TEXT("KeyTypeProperties"));
}

// An edit of the key itself that leaves its key-type properties as stored: its Description and Category.
void DescribeKey(const TSharedPtr<FJsonObject>& Document, const int32 Index)
{
	const TSharedPtr<FJsonObject> Entry = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Keys"))[Index]->AsObject();
	Entry->SetStringField(TEXT("Description"), TEXT("Described"));
	Entry->SetStringField(TEXT("Category"), TEXT("Tolerated"));
}

bool IsDescribed(const FBlackboardEntry* Entry)
{
	return Entry != nullptr && Entry->EntryDescription == TEXT("Described") && Entry->EntryCategory == TEXT("Tolerated");
}

const FBlackboardEntry* FindBoardKey(const UBlackboardData* Board, const TCHAR* Name)
{
	return Board == nullptr ? nullptr
		: Board->Keys.FindByPredicate([Name](const FBlackboardEntry& Entry) { return Entry.EntryName == Name; });
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBlackboardEnumKeysTest, "UEAssetDoc.Tolerant.BlackboardEnumKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBlackboardEnumKeysTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBlackboardKeyTestsLocal;
	const FTarget Target(TEXT("BB_EnumKeys"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlackboardData* Board = Target.Create<UBlackboardData>();
	// A work-in-progress Enum key: no enum picked yet.
	AddBoardKey<UBlackboardKeyType_Enum>(*Board, TEXT("Unset"));
	// Enum Name typed as the C++ enum's short name in the details panel; the engine resolves EnumType from it.
	UBlackboardKeyType_Enum* Movement = AddBoardKey<UBlackboardKeyType_Enum>(*Board, TEXT("Movement"));
	Movement->EnumName = TEXT("EMovementMode");
	// The engine resolves a short name with a deprecation warning.
	AddExpectedMessage(TEXT("Short type name \"EMovementMode\""), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	FPropertyChangedEvent EnumNameChanged(FindFProperty<FProperty>(UBlackboardKeyType_Enum::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Enum, EnumName)));
	static_cast<UObject*>(Movement)->PostEditChangeProperty(EnumNameChanged);
	Movement->DefaultValue = static_cast<uint8>(MOVE_Walking);
	// A default whose enumerator no longer exists.
	UBlackboardKeyType_Enum* Stale = AddBoardKey<UBlackboardKeyType_Enum>(*Board, TEXT("Stale"));
	Stale->EnumType = StaticEnum<EMovementMode>();
	Stale->EnumName = Stale->EnumType->GetPathName();
	Stale->bIsEnumNameValid = true;
	Stale->DefaultValue = 200;
	Board->UpdateParentKeys();
	if (!TestTrue(TEXT("short Enum Name resolves"), Movement->EnumType.Get() == StaticEnum<EMovementMode>())
		|| !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("BlackboardData.MissingEnumType"), TEXT("/Body/Keys/0/KeyTypeProperties/EnumType")},
		{TEXT("BlackboardData.InvalidEnumDefault"), TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue")},
		{TEXT("BlackboardData.InvalidEntryField"), TEXT("/Body/Keys/1/KeyTypeProperties/EnumName")},
		{TEXT("BlackboardData.InvalidEnumDefault"), TEXT("/Body/Keys/2/KeyTypeProperties/DefaultValue")}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Enum keys"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("EnumName as stored"), StringAt(Extracted, TEXT("/Body/Keys/1/KeyTypeProperties/EnumName")), FString(TEXT("EMovementMode")));
	const TSharedPtr<FJsonValue> RawDefault = At(Extracted, TEXT("/Body/Keys/2/KeyTypeProperties/DefaultValue"));
	TestTrue(TEXT("a default that is no enumerator is its raw value"), RawDefault.IsValid() && RawDefault->Type == EJson::Number
		&& static_cast<int32>(RawDefault->AsNumber()) == 200);

	if (TestTrue(TEXT("another key is added"), ApplyEdited(*this, Target, AppendBoolKey, TEXT("Edit")).IsSuccess()))
	{
		const UBlackboardData* Edited = Target.Get<UBlackboardData>();
		const FBlackboardEntry* StaleEntry = FindBoardKey(Edited, TEXT("Stale"));
		const FBlackboardEntry* MovementEntry = FindBoardKey(Edited, TEXT("Movement"));
		TestTrue(TEXT("anomalous Enum keys kept"), StaleEntry != nullptr && MovementEntry != nullptr && FindBoardKey(Edited, TEXT("Unset")) != nullptr
			&& CastChecked<UBlackboardKeyType_Enum>(StaleEntry->KeyType)->DefaultValue == 200
			&& CastChecked<UBlackboardKeyType_Enum>(MovementEntry->KeyType)->EnumName == TEXT("EMovementMode"));
		TestStillShows(*this, Target, TEXT("BlackboardData.MissingEnumType"), TEXT("/Body/Keys/0/KeyTypeProperties/EnumType"), TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("BlackboardData.InvalidEntryField"), TEXT("/Body/Keys/1/KeyTypeProperties/EnumName"), TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("BlackboardData.InvalidEnumDefault"), TEXT("/Body/Keys/2/KeyTypeProperties/DefaultValue"), TEXT("After edit"));
	}
	// The anomalous keys themselves take a Description and Category; their values stay as stored.
	if (TestTrue(TEXT("the anomalous Enum keys are described"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		DescribeKey(Document, 0);
		DescribeKey(Document, 2);
	}, TEXT("Describe")).IsSuccess()))
	{
		const UBlackboardData* Edited = Target.Get<UBlackboardData>();
		const FBlackboardEntry* UnsetEntry = FindBoardKey(Edited, TEXT("Unset"));
		const FBlackboardEntry* StaleEntry = FindBoardKey(Edited, TEXT("Stale"));
		TestTrue(TEXT("described with their values kept"), IsDescribed(UnsetEntry) && IsDescribed(StaleEntry)
			&& CastChecked<UBlackboardKeyType_Enum>(UnsetEntry->KeyType)->EnumType == nullptr
			&& CastChecked<UBlackboardKeyType_Enum>(StaleEntry->KeyType)->DefaultValue == 200);
		TestStillShows(*this, Target, TEXT("BlackboardData.InvalidEnumDefault"), TEXT("/Body/Keys/2/KeyTypeProperties/DefaultValue"), TEXT("After describe"));
	}
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		KeyTypePropertiesAt(Document, 2)->SetNumberField(TEXT("DefaultValue"), 201);
	}, TEXT("Changed raw default")), TEXT("BlackboardData.InvalidEnumDefault"), TEXT("/Body/Keys/2/KeyTypeProperties/DefaultValue"),
		TEXT("Changed raw default"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBlackboardBaseClassKeysTest, "UEAssetDoc.Tolerant.BlackboardBaseClassKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBlackboardBaseClassKeysTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBlackboardKeyTestsLocal;
	const FTarget Target(TEXT("BB_BaseClassKeys"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// Defaults set first, then BaseClass narrowed; nothing clears the default.
	UBlackboardData* Board = Target.Create<UBlackboardData>();
	UBlackboardKeyType_Object* Picked = AddBoardKey<UBlackboardKeyType_Object>(*Board, TEXT("Target"));
	Picked->DefaultValue = GetMutableDefault<AActor>();
	Picked->BaseClass = APawn::StaticClass();
	UBlackboardKeyType_Class* Spawn = AddBoardKey<UBlackboardKeyType_Class>(*Board, TEXT("Spawn"));
	Spawn->DefaultValue = AActor::StaticClass();
	Spawn->BaseClass = APawn::StaticClass();
	// A key whose BaseClass is gone (the class it named was deleted).
	AddBoardKey<UBlackboardKeyType_Object>(*Board, TEXT("Unbased"))->BaseClass = nullptr;
	Board->UpdateParentKeys();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("BlackboardData.DefaultValueNotOfBaseClass"), TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue")},
		{TEXT("BlackboardData.DefaultValueNotOfBaseClass"), TEXT("/Body/Keys/1/KeyTypeProperties/DefaultValue")},
		{TEXT("BlackboardData.MissingBaseClass"), TEXT("/Body/Keys/2/KeyTypeProperties/BaseClass")}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("BaseClass keys"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("both values read: BaseClass"), StringAt(Extracted, TEXT("/Body/Keys/1/KeyTypeProperties/BaseClass/Path")),
		APawn::StaticClass()->GetPathName());
	TestEqual(TEXT("both values read: DefaultValue"), StringAt(Extracted, TEXT("/Body/Keys/1/KeyTypeProperties/DefaultValue/Path")),
		AActor::StaticClass()->GetPathName());

	if (TestTrue(TEXT("another key is added"), ApplyEdited(*this, Target, AppendBoolKey, TEXT("Edit")).IsSuccess()))
	{
		const FBlackboardEntry* SpawnEntry = FindBoardKey(Target.Get<UBlackboardData>(), TEXT("Spawn"));
		TestTrue(TEXT("mismatched default kept"), SpawnEntry != nullptr
			&& CastChecked<UBlackboardKeyType_Class>(SpawnEntry->KeyType)->DefaultValue == AActor::StaticClass());
		TestStillShows(*this, Target, TEXT("BlackboardData.DefaultValueNotOfBaseClass"), TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue"), TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("BlackboardData.DefaultValueNotOfBaseClass"), TEXT("/Body/Keys/1/KeyTypeProperties/DefaultValue"), TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("BlackboardData.MissingBaseClass"), TEXT("/Body/Keys/2/KeyTypeProperties/BaseClass"), TEXT("After edit"));
	}
	// The anomalous keys themselves take a Description and Category; their values stay as stored.
	if (TestTrue(TEXT("the anomalous Object and Class keys are described"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		DescribeKey(Document, 1);
		DescribeKey(Document, 2);
	}, TEXT("Describe")).IsSuccess()))
	{
		const UBlackboardData* Edited = Target.Get<UBlackboardData>();
		const FBlackboardEntry* SpawnEntry = FindBoardKey(Edited, TEXT("Spawn"));
		const FBlackboardEntry* Unbased = FindBoardKey(Edited, TEXT("Unbased"));
		TestTrue(TEXT("described with their values kept"), IsDescribed(SpawnEntry) && IsDescribed(Unbased)
			&& CastChecked<UBlackboardKeyType_Class>(SpawnEntry->KeyType)->DefaultValue == AActor::StaticClass()
			&& CastChecked<UBlackboardKeyType_Object>(Unbased->KeyType)->BaseClass == nullptr);
	}
	// Authoring a BaseClass for the key that has none is an ordinary edit.
	if (TestTrue(TEXT("a BaseClass is authored"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		KeyTypePropertiesAt(Document, 2)->SetObjectField(TEXT("BaseClass"), Parse(TEXT("{\"Kind\":\"ClassRef\",\"Path\":\"/Script/Engine.Actor\"}")));
	}, TEXT("Fix BaseClass")).IsSuccess()))
	{
		const FBlackboardEntry* Unbased = FindBoardKey(Target.Get<UBlackboardData>(), TEXT("Unbased"));
		TestTrue(TEXT("BaseClass written"), Unbased != nullptr
			&& CastChecked<UBlackboardKeyType_Object>(Unbased->KeyType)->BaseClass == AActor::StaticClass());
	}
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		KeyTypePropertiesAt(Document, 2)->SetField(TEXT("BaseClass"), MakeShared<FJsonValueNull>());
	}, TEXT("Cleared BaseClass")), TEXT("BlackboardData.MissingBaseClass"), TEXT("/Body/Keys/2/KeyTypeProperties/BaseClass"),
		TEXT("Cleared BaseClass"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBlackboardProjectKeyTypeTest, "UEAssetDoc.Tolerant.BlackboardProjectKeyType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBlackboardProjectKeyTypeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBlackboardKeyTestsLocal;
	const FTarget Target(TEXT("BB_ProjectKeyType"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A project key type with array, struct and other non-scalar properties.
	UClass* KeyClass = FindObject<UClass>(nullptr, ProjectKeyTypeClass);
	if (!TestNotNull(TEXT("the test key type class is loaded"), KeyClass))
	{
		return false;
	}
	UBlackboardData* Board = Target.Create<UBlackboardData>();
	UBlackboardKeyType* Tagged = AddBoardKey<UBlackboardKeyType>(*Board, TEXT("Tagged"), KeyClass);
	FProperty* ArrayProperty = KeyClass->FindPropertyByName(TEXT("UnsupportedArrayValue"));
	if (!TestNotNull(TEXT("array property"), ArrayProperty)
		|| !TestNotNull(TEXT("array imported"), ArrayProperty->ImportText_InContainer(TEXT("(1,2,3)"), Tagged, Tagged, PPF_None)))
	{
		return false;
	}
	Board->UpdateParentKeys();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Project key type"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	auto ArrayLength = [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Value = At(Document, TEXT("/Body/Keys/0/KeyTypeProperties/UnsupportedArrayValue"));
		return Value.IsValid() && Value->Type == EJson::Array ? Value->AsArray().Num() : -1;
	};
	TestEqual(TEXT("array property read in the shared codec's form"), ArrayLength(Extracted), 3);
	const TSharedPtr<FJsonValue> StructValue = At(Extracted, TEXT("/Body/Keys/0/KeyTypeProperties/UnsupportedStructValue"));
	TestTrue(TEXT("struct property read in the shared codec's form"), StructValue.IsValid() && StructValue->Type == EJson::Object);

	if (TestTrue(TEXT("another key is added"), ApplyEdited(*this, Target, AppendBoolKey, TEXT("Edit")).IsSuccess()))
	{
		const FAssetDocumentResult After = ExtractResult(Target);
		TestTrue(TEXT("array property kept"), After.IsSuccess() && After.Payload.IsValid() && ArrayLength(After.Payload) == 3);
	}
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Shorter;
		Shorter.Add(MakeShared<FJsonValueNumber>(1));
		KeyTypePropertiesAt(Document, 0)->SetArrayField(TEXT("UnsupportedArrayValue"), Shorter);
	}, TEXT("Changed array")), TEXT("BlackboardData.KeyTypePropertyReadOnly"), TEXT("/Body/Keys/0/KeyTypeProperties/UnsupportedArrayValue"),
		TEXT("Changed array"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBlackboardStructKeysTest, "UEAssetDoc.Tolerant.BlackboardStructKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBlackboardStructKeysTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBlackboardKeyTestsLocal;
	const FTarget Target(TEXT("BB_StructKeys"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, LabelledStruct);
	FTextProperty* TextProperty = Struct != nullptr ? CastField<FTextProperty>(Struct->FindPropertyByName(TEXT("TextValue"))) : nullptr;
	FProperty* EnumProperty = Struct != nullptr ? Struct->FindPropertyByName(TEXT("EnumValue")) : nullptr;
	if (!TestNotNull(TEXT("the test struct is loaded"), Struct) || !TestNotNull(TEXT("text member"), TextProperty)
		|| !TestNotNull(TEXT("enum member"), EnumProperty))
	{
		return false;
	}
	UBlackboardData* Board = Target.Create<UBlackboardData>();
	// A text member typed in the details panel: a localized text with its own key.
	UBlackboardKeyType_Struct* Label = AddBoardKey<UBlackboardKeyType_Struct>(*Board, TEXT("Label"));
	Label->DefaultValue.InitializeAs(Struct);
	TextProperty->SetPropertyValue_InContainer(Label->DefaultValue.GetMutableMemory(),
		FText::ChangeKey(TEXT("AssetDocTolerant"), TEXT("Greeting"), FText::FromString(TEXT("Hello"))));
	// An enum member set to an enumerator hidden from the dropdown.
	UBlackboardKeyType_Struct* Mode = AddBoardKey<UBlackboardKeyType_Struct>(*Board, TEXT("Mode"));
	Mode->DefaultValue.InitializeAs(Struct);
	if (!TestNotNull(TEXT("hidden enumerator imported"),
		EnumProperty->ImportText_InContainer(TEXT("HiddenValue"), Mode->DefaultValue.GetMutableMemory(), Mode, PPF_None)))
	{
		return false;
	}
	Board->UpdateParentKeys();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("BlackboardData.Unrepresented"), TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue")},
		{TEXT("BlackboardData.Unrepresented"), TEXT("/Body/Keys/1/KeyTypeProperties/DefaultValue")}};
	Expected.Markers = {TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue"), TEXT("/Body/Keys/1/KeyTypeProperties/DefaultValue")};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Struct keys"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("marker text names the localized text"),
		StringAt(Extracted, TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue/Text")).Contains(TEXT("Greeting")));

	if (TestTrue(TEXT("another key is added"), ApplyEdited(*this, Target, AppendBoolKey, TEXT("Edit")).IsSuccess()))
	{
		const FBlackboardEntry* LabelEntry = FindBoardKey(Target.Get<UBlackboardData>(), TEXT("Label"));
		const UBlackboardKeyType_Struct* LabelKey = LabelEntry != nullptr ? Cast<UBlackboardKeyType_Struct>(LabelEntry->KeyType) : nullptr;
		const TOptional<FString> TextKey = LabelKey != nullptr && LabelKey->DefaultValue.GetScriptStruct() == Struct
			? FTextInspector::GetKey(TextProperty->GetPropertyValue_InContainer(LabelKey->DefaultValue.GetMemory())) : TOptional<FString>();
		TestTrue(TEXT("localized text kept with its key"), TextKey.IsSet() && TextKey.GetValue() == TEXT("Greeting"));
		TestStillShows(*this, Target, TEXT("BlackboardData.Unrepresented"), TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue"), TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("BlackboardData.Unrepresented"), TEXT("/Body/Keys/1/KeyTypeProperties/DefaultValue"), TEXT("After edit"));
	}
	// The marked key itself takes a Description and Category; its value stays as stored.
	if (TestTrue(TEXT("the marked Struct key is described"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		DescribeKey(Document, 0);
	}, TEXT("Describe")).IsSuccess()))
	{
		const FBlackboardEntry* LabelEntry = FindBoardKey(Target.Get<UBlackboardData>(), TEXT("Label"));
		const UBlackboardKeyType_Struct* LabelKey = LabelEntry != nullptr ? Cast<UBlackboardKeyType_Struct>(LabelEntry->KeyType) : nullptr;
		const TOptional<FString> TextKey = LabelKey != nullptr && LabelKey->DefaultValue.GetScriptStruct() == Struct
			? FTextInspector::GetKey(TextProperty->GetPropertyValue_InContainer(LabelKey->DefaultValue.GetMemory())) : TOptional<FString>();
		TestTrue(TEXT("described with its localized text kept"), IsDescribed(LabelEntry) && TextKey.IsSet() && TextKey.GetValue() == TEXT("Greeting"));
		TestStillShows(*this, Target, TEXT("BlackboardData.Unrepresented"), TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue"), TEXT("After describe"));
	}
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		KeyTypePropertiesAt(Document, 0)->GetObjectField(TEXT("DefaultValue"))->SetStringField(TEXT("Text"), TEXT("(edited)"));
	}, TEXT("Edited marker")), TEXT("BlackboardData.UnrepresentedValue"), TEXT("/Body/Keys/0/KeyTypeProperties/DefaultValue"),
		TEXT("Edited marker"));
	return !HasAnyErrors();
}

#endif
