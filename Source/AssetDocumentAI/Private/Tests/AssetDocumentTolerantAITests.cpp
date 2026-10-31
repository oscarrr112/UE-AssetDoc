// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Blackboard whose local key shadows a key of its Parent (keys made first, Parent set later; the
// engine keeps both): it reads with a warning, its own extract re-applies as a no-op, adding, reordering and removing
// other keys works, and a new shadow is still refused.

#include "AssetDocumentTolerantTestSupport.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/BlackboardData.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantAITestsLocal
{
void AddKey(UBlackboardData& Board, const TCHAR* Name, UClass* KeyType)
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	Entry.KeyType = NewObject<UBlackboardKeyType>(&Board, KeyType, NAME_None, RF_Transactional);
	Board.Keys.Add(Entry);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBlackboardShadowTest, "UEAssetDoc.Tolerant.BlackboardShadow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBlackboardShadowTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantAITestsLocal;
	const FTarget Base(TEXT("BB_Base"));
	const FTarget Child(TEXT("BB_Child"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Child.Cleanup();
		Base.Cleanup();
	};

	UBlackboardData* BaseBoard = Base.Create<UBlackboardData>();
	AddKey(*BaseBoard, TEXT("Target"), UBlackboardKeyType_Object::StaticClass());
	AddKey(*BaseBoard, TEXT("Home"), UBlackboardKeyType_Object::StaticClass());
	BaseBoard->UpdateParentKeys();
	UBlackboardData* ChildBoard = Child.Create<UBlackboardData>();
	AddKey(*ChildBoard, TEXT("Target"), UBlackboardKeyType_Object::StaticClass());
	ChildBoard->UpdateParentKeys();
	// The Parent field of the Blackboard's details panel, set after the child's keys were made.
	ChildBoard->Modify();
	ChildBoard->Parent = BaseBoard;
	ChildBoard->UpdateParentKeys();
	if (!TestTrue(TEXT("base saved"), Base.Save()) || !TestTrue(TEXT("child saved"), Child.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {{TEXT("BlackboardData.InheritedShadow"), TEXT("/Body/Keys/0/Name")}};
	if (!TestTolerantRoundTrip(*this, Child, Expected, TEXT("Shadowing child")).IsValid())
	{
		return false;
	}

	auto Append = [](const TCHAR* Name, const TCHAR* KeyType)
	{
		return [Name, KeyType](const TSharedPtr<FJsonObject>& Document)
		{
			TArray<TSharedPtr<FJsonValue>> Keys = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Keys"));
			const TSharedPtr<FJsonObject> Key = Parse(Serialize(Keys[0]->AsObject()));
			Key->SetStringField(TEXT("Name"), Name);
			Key->GetObjectField(TEXT("KeyTypeClass"))->SetStringField(TEXT("Path"), KeyType);
			Keys.Add(MakeShared<FJsonValueObject>(Key));
			Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Keys"), Keys);
		};
	};
	if (TestTrue(TEXT("another key is added"), ApplyEdited(*this, Child,
		Append(TEXT("Alerted"), TEXT("/Script/AIModule.BlackboardKeyType_Object")), TEXT("Edit")).IsSuccess()))
	{
		ChildBoard = Child.Get<UBlackboardData>();
		TestTrue(TEXT("shadowing key kept"), ChildBoard != nullptr && ChildBoard->Keys.ContainsByPredicate(
			[](const FBlackboardEntry& Entry) { return Entry.EntryName == TEXT("Target"); }));
		TestStillShows(*this, Child, TEXT("BlackboardData.InheritedShadow"), TEXT("/Body/Keys/0/Name"), TEXT("After edit"));
	}
	auto LocalKeyNames = [&Child]()
	{
		TArray<FString> Names;
		if (const UBlackboardData* Board = Child.Get<UBlackboardData>())
		{
			for (const FBlackboardEntry& Entry : Board->Keys)
			{
				Names.Add(Entry.EntryName.ToString());
			}
		}
		return FString::Join(Names, TEXT(","));
	};
	// Reorder the local keys (the shadowing key moves down), then remove the other key.
	if (TestTrue(TEXT("the keys are reordered"), ApplyEdited(*this, Child, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Keys = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Keys"));
		Keys.Swap(0, 1);
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Keys"), Keys);
	}, TEXT("Reorder")).IsSuccess()))
	{
		TestEqual(TEXT("reordered local keys"), LocalKeyNames(), FString(TEXT("Alerted,Target")));
		TestStillShows(*this, Child, TEXT("BlackboardData.InheritedShadow"), TEXT("/Body/Keys/1/Name"), TEXT("After reorder"));
	}
	if (TestTrue(TEXT("the other key is removed"), ApplyEdited(*this, Child, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Keys = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Keys"));
		Keys.RemoveAll([](const TSharedPtr<FJsonValue>& Key) { return Key->AsObject()->GetStringField(TEXT("Name")) == TEXT("Alerted"); });
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Keys"), Keys);
	}, TEXT("Remove")).IsSuccess()))
	{
		TestEqual(TEXT("the shadowing key is left"), LocalKeyNames(), FString(TEXT("Target")));
		TestStillShows(*this, Child, TEXT("BlackboardData.InheritedShadow"), TEXT("/Body/Keys/0/Name"), TEXT("After remove"));
	}
	TestRefused(*this, ApplyEdited(*this, Child, Append(TEXT("Home"), TEXT("/Script/AIModule.BlackboardKeyType_Object")),
		TEXT("New shadow")), TEXT("BlackboardData.InheritedShadow"), TEXT("/Body/Keys/1/Name"), TEXT("New shadow"));
	return !HasAnyErrors();
}

#endif
