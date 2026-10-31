// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A Behavior Tree whose Wait task carries a Tag Cooldown decorator and a Check Gameplay Tags On Actor decorator, their
// tags picked in the decorators' Details (Cooldown Tag; Gameplay Tags, two tags): Extract shows the tag names, the
// extract re-applies as a no-op, a Tree apply that adds a node (the whole graph is rebuilt) keeps both decorators' tags,
// and a document edits the tag and the tag container. The tags are registered for the test from a tag list file, as a
// project's Config/Tags list registers them.

#include "AssetDocumentTolerantTestSupport.h"

#include "AIGraph.h"
#include "AIGraphNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Decorators/BTDecorator_CheckGameplayTagsOnActor.h"
#include "BehaviorTree/Decorators/BTDecorator_TagCooldown.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const BehaviorTreeTagAttack = TEXT("AssetDocumentTest.Cooldown.Attack");
const TCHAR* const BehaviorTreeTagDodge = TEXT("AssetDocumentTest.Cooldown.Dodge");
const TCHAR* const BehaviorTreeTagStunned = TEXT("AssetDocumentTest.State.Stunned");
const TCHAR* const BehaviorTreeTagRooted = TEXT("AssetDocumentTest.State.Rooted");
const TCHAR* const BehaviorTreeCooldownClass = TEXT("/Script/AIModule.BTDecorator_TagCooldown");
const TCHAR* const BehaviorTreeCheckTagsClass = TEXT("/Script/AIModule.BTDecorator_CheckGameplayTagsOnActor");

// The test's gameplay tags, from a tag list file in a tag search path of its own (Saved/), for the scope.
struct FBehaviorTreeTagListScope
{
	FString Directory;

	FBehaviorTreeTagListScope()
		: Directory(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), AssetDocumentTolerantTestSupport::FolderName,
			FString::Printf(TEXT("Tags_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))))
	{
		FString List = TEXT("[/Script/GameplayTags.GameplayTagsList]\n");
		for (const TCHAR* Tag : {BehaviorTreeTagAttack, BehaviorTreeTagDodge, BehaviorTreeTagStunned, BehaviorTreeTagRooted})
		{
			List += FString::Printf(TEXT("GameplayTagList=(Tag=\"%s\",DevComment=\"\")\n"), Tag);
		}
		IFileManager::Get().MakeDirectory(*Directory, true);
		FFileHelper::SaveStringToFile(List, *FPaths::Combine(Directory, TEXT("AssetDocumentTestTags.ini")));
		UGameplayTagsManager::Get().AddTagIniSearchPath(Directory);
	}

	~FBehaviorTreeTagListScope()
	{
		// The engine keeps the tags for the session when it does not unload tags.
		UGameplayTagsManager::Get().RemoveTagIniSearchPath(Directory);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	}

	FBehaviorTreeTagListScope(const FBehaviorTreeTagListScope&) = delete;
	FBehaviorTreeTagListScope& operator=(const FBehaviorTreeTagListScope&) = delete;
};

FGameplayTag BehaviorTreeTag(const TCHAR* Name)
{
	return UGameplayTagsManager::Get().RequestGameplayTag(FName(Name), false);
}

TSharedPtr<FJsonObject> BehaviorTreeTagsDocument(const FString& Text)
{
	return AssetDocumentTolerantTestSupport::Parse(Text);
}

// The decorator object of ClassPath anywhere under the document's Tree, or null.
TSharedPtr<FJsonObject> BehaviorTreeDecoratorIn(const TSharedPtr<FJsonObject>& Node, const FString& ClassPath)
{
	if (!Node.IsValid())
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Decorators = nullptr;
	if (Node->TryGetArrayField(TEXT("Decorators"), Decorators))
	{
		for (const TSharedPtr<FJsonValue>& Decorator : *Decorators)
		{
			FString Class;
			if (Decorator->Type == EJson::Object && Decorator->AsObject()->TryGetStringField(TEXT("Class"), Class) && Class == ClassPath)
			{
				return Decorator->AsObject();
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Node->TryGetArrayField(TEXT("Children"), Children))
	{
		for (const TSharedPtr<FJsonValue>& Child : *Children)
		{
			if (TSharedPtr<FJsonObject> Found = BehaviorTreeDecoratorIn(Child->Type == EJson::Object ? Child->AsObject() : nullptr, ClassPath))
			{
				return Found;
			}
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> BehaviorTreeDecorator(const TSharedPtr<FJsonObject>& Document, const FString& ClassPath)
{
	const TSharedPtr<FJsonValue> Root = AssetDocumentTolerantTestSupport::At(Document, TEXT("/Body/Tree/Root"));
	return BehaviorTreeDecoratorIn(Root.IsValid() && Root->Type == EJson::Object ? Root->AsObject() : nullptr, ClassPath);
}

// A decorator's tag property as the document shows it: a tag name, or the names joined with ",", or "<absent>".
FString BehaviorTreeDocumentTags(const TSharedPtr<FJsonObject>& Document, const FString& ClassPath, const TCHAR* Property)
{
	const TSharedPtr<FJsonObject> Decorator = BehaviorTreeDecorator(Document, ClassPath);
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!Decorator.IsValid() || !Decorator->TryGetObjectField(TEXT("Properties"), Properties))
	{
		return TEXT("<absent>");
	}
	const TSharedPtr<FJsonValue> Value = (*Properties)->TryGetField(Property);
	FString Text;
	if (Value.IsValid() && Value->TryGetString(Text))
	{
		return Text;
	}
	if (!Value.IsValid() || Value->Type != EJson::Array)
	{
		return TEXT("<absent>");
	}
	TArray<FString> Names;
	for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
	{
		Names.Add(Item->TryGetString(Text) ? Text : FString(TEXT("<not a name>")));
	}
	return FString::Join(Names, TEXT(","));
}

// A decorator's tag property on the object: the tag name, or the container's names joined with ",".
FString BehaviorTreeNodeTags(const UObject* Decorator, const TCHAR* Property)
{
	const FStructProperty* Field = Decorator != nullptr ? FindFProperty<FStructProperty>(Decorator->GetClass(), Property) : nullptr;
	if (Field == nullptr)
	{
		return TEXT("<no decorator>");
	}
	if (Field->Struct == FGameplayTag::StaticStruct())
	{
		return Field->ContainerPtrToValuePtr<FGameplayTag>(Decorator)->GetTagName().ToString();
	}
	TArray<FString> Names;
	for (const FGameplayTag& Tag : *Field->ContainerPtrToValuePtr<FGameplayTagContainer>(Decorator))
	{
		Names.Add(Tag.GetTagName().ToString());
	}
	return FString::Join(Names, TEXT(","));
}

// The editor graph's decorator instance of Class (what the Details panel edits), or null.
UBTDecorator* BehaviorTreeGraphDecorator(const UBehaviorTree* Tree, const UClass* Class)
{
	const UAIGraph* Graph = Tree != nullptr ? Cast<UAIGraph>(Tree->BTGraph) : nullptr;
	if (Graph == nullptr)
	{
		return nullptr;
	}
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		const UAIGraphNode* AINode = Cast<UAIGraphNode>(Node);
		for (int32 Index = 0; AINode != nullptr && Index < AINode->SubNodes.Num(); ++Index)
		{
			const UAIGraphNode* SubNode = AINode->SubNodes[Index];
			if (SubNode != nullptr && SubNode->NodeInstance != nullptr && SubNode->NodeInstance->IsA(Class))
			{
				return Cast<UBTDecorator>(SubNode->NodeInstance);
			}
		}
	}
	return nullptr;
}

// The runtime tree's decorator of Class on a child of the root (what the AI runs), or null.
const UBTDecorator* BehaviorTreeRuntimeDecorator(const UBehaviorTree* Tree, const UClass* Class)
{
	const UBTCompositeNode* Root = Tree != nullptr ? Tree->RootNode.Get() : nullptr;
	for (int32 Child = 0; Root != nullptr && Child < Root->Children.Num(); ++Child)
	{
		for (const UBTDecorator* Decorator : Root->Children[Child].Decorators)
		{
			if (Decorator != nullptr && Decorator->IsA(Class))
			{
				return Decorator;
			}
		}
	}
	return nullptr;
}

// The Details panel's edit of a decorator's tag property.
void BehaviorTreePickTags(UBTDecorator& Decorator, const TCHAR* Property, const TArray<const TCHAR*>& Tags)
{
	FStructProperty* Field = FindFProperty<FStructProperty>(Decorator.GetClass(), Property);
	if (Field == nullptr)
	{
		return;
	}
	Decorator.Modify();
	if (Field->Struct == FGameplayTag::StaticStruct())
	{
		*Field->ContainerPtrToValuePtr<FGameplayTag>(&Decorator) = Tags.Num() > 0 ? BehaviorTreeTag(Tags[0]) : FGameplayTag();
	}
	else
	{
		FGameplayTagContainer& Container = *Field->ContainerPtrToValuePtr<FGameplayTagContainer>(&Decorator);
		Container.Reset();
		for (const TCHAR* Tag : Tags)
		{
			Container.AddTag(BehaviorTreeTag(Tag));
		}
	}
	FPropertyChangedEvent Event(Field, EPropertyChangeType::ValueSet);
	Decorator.PostEditChangeProperty(Event);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCodecBehaviorTreeGameplayTagsTest, "UEAssetDoc.Codec.BehaviorTree.GameplayTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCodecBehaviorTreeGameplayTagsTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	const FBehaviorTreeTagListScope TagList;
	const Tolerant::FTarget BlackboardTarget(TEXT("BB_GameplayTags"));
	const Tolerant::FTarget Target(TEXT("BT_GameplayTags"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		BlackboardTarget.Cleanup();
	};
	for (const TCHAR* Tag : {BehaviorTreeTagAttack, BehaviorTreeTagDodge, BehaviorTreeTagStunned, BehaviorTreeTagRooted})
	{
		if (!TestTrue(*FString::Printf(TEXT("tag %s is registered"), Tag), BehaviorTreeTag(Tag).IsValid()))
		{
			return false;
		}
	}

	// A Blackboard with an actor key, and a tree whose Wait task has the two decorators.
	const FAssetDocumentResult Board = Tolerant::ApplyText(*this, BlackboardTarget, FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/AIModule.BlackboardData", "Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"Parent": null, "Keys": [
		{"Name": "Target", "Category": "", "Description": "", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Object"},
		 "KeyTypeProperties": {"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "DefaultValue": null}}]}})"),
		*BlackboardTarget.PackageName), TEXT("Blackboard"));
	if (!TestTrue(TEXT("blackboard created"), Board.IsSuccess()))
	{
		return false;
	}
	const FAssetDocumentResult Created = Tolerant::ApplyText(*this, Target, FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/AIModule.BehaviorTree", "Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {
		"BlackboardAsset": {"Kind": "AssetRef", "Path": "%s"},
		"Tree": {"Root": {"Id": "00000000000000000000000000000001", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
			{"Id": "00000000000000000000000000000002", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 0, "Y": 200}},
			 "Decorators": [
				{"Id": "00000000000000000000000000000003", "Class": "%s"},
				{"Id": "00000000000000000000000000000004", "Class": "%s", "Properties": {"ActorToCheck": {"Key": "Target"}}}]}]}}}})"),
		*Target.PackageName, *BlackboardTarget.ObjectPath, BehaviorTreeCooldownClass, BehaviorTreeCheckTagsClass), TEXT("Behavior Tree"));
	if (!TestTrue(TEXT("behavior tree created"), Created.IsSuccess()))
	{
		return false;
	}

	// The decorators' Details: Cooldown Tag, and two Gameplay Tags; the editor then updates the tree and it is saved.
	UBehaviorTree* Tree = Target.Get<UBehaviorTree>();
	UBTDecorator* Cooldown = BehaviorTreeGraphDecorator(Tree, UBTDecorator_TagCooldown::StaticClass());
	UBTDecorator* CheckTags = BehaviorTreeGraphDecorator(Tree, UBTDecorator_CheckGameplayTagsOnActor::StaticClass());
	UAIGraph* Graph = Tree != nullptr ? Cast<UAIGraph>(Tree->BTGraph) : nullptr;
	if (!TestNotNull(TEXT("tree graph"), Graph) || !TestNotNull(TEXT("Tag Cooldown decorator"), Cooldown)
		|| !TestNotNull(TEXT("Check Gameplay Tags On Actor decorator"), CheckTags))
	{
		return false;
	}
	BehaviorTreePickTags(*Cooldown, TEXT("CooldownTag"), {BehaviorTreeTagAttack});
	BehaviorTreePickTags(*CheckTags, TEXT("GameplayTags"), {BehaviorTreeTagStunned, BehaviorTreeTagRooted});
	Graph->UpdateAsset();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const FString Container = FString::Printf(TEXT("%s,%s"), BehaviorTreeTagStunned, BehaviorTreeTagRooted);
	TestEqual(TEXT("the runtime cooldown decorator has the tag"),
		BehaviorTreeNodeTags(BehaviorTreeRuntimeDecorator(Tree, UBTDecorator_TagCooldown::StaticClass()), TEXT("CooldownTag")), FString(BehaviorTreeTagAttack));

	// Extract shows the tag names; the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Tagged tree"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("CooldownTag reads as its name"), BehaviorTreeDocumentTags(Extracted, BehaviorTreeCooldownClass, TEXT("CooldownTag")),
		FString(BehaviorTreeTagAttack));
	TestEqual(TEXT("GameplayTags reads as the names in order"), BehaviorTreeDocumentTags(Extracted, BehaviorTreeCheckTagsClass, TEXT("GameplayTags")),
		Container);

	// A Tree apply that adds a task after the Wait task rebuilds every node; both decorators keep their tags.
	const FString AddedId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (TestTrue(TEXT("a task is added"), Tolerant::ApplyEdited(*this, Target, [&AddedId](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> RootValue = Tolerant::At(Document, TEXT("/Body/Tree/Root"));
		const TSharedPtr<FJsonValue> X = Tolerant::At(Document, TEXT("/Body/Tree/Root/Children/0/Editor/Position/X"));
		double FirstX = 0.0;
		if (!RootValue.IsValid() || RootValue->Type != EJson::Object || !X.IsValid() || !X->TryGetNumber(FirstX))
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Children = RootValue->AsObject()->GetArrayField(TEXT("Children"));
		Children.Add(MakeShared<FJsonValueObject>(BehaviorTreeTagsDocument(FString::Printf(TEXT(R"({"Id": "%s",
			"Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": %d, "Y": 200}}})"),
			*AddedId, static_cast<int32>(FirstX) + 400))));
		RootValue->AsObject()->SetArrayField(TEXT("Children"), Children);
	}, TEXT("Add a task")).IsSuccess()))
	{
		Tree = Target.Get<UBehaviorTree>();
		TestTrue(TEXT("the tree has two tasks"), Tree != nullptr && Tree->RootNode != nullptr && Tree->RootNode->Children.Num() == 2);
		TestEqual(TEXT("the cooldown decorator keeps its tag"),
			BehaviorTreeNodeTags(BehaviorTreeGraphDecorator(Tree, UBTDecorator_TagCooldown::StaticClass()), TEXT("CooldownTag")), FString(BehaviorTreeTagAttack));
		TestEqual(TEXT("the check decorator keeps its tags"),
			BehaviorTreeNodeTags(BehaviorTreeGraphDecorator(Tree, UBTDecorator_CheckGameplayTagsOnActor::StaticClass()), TEXT("GameplayTags")), Container);
		TestEqual(TEXT("the runtime cooldown decorator keeps its tag"),
			BehaviorTreeNodeTags(BehaviorTreeRuntimeDecorator(Tree, UBTDecorator_TagCooldown::StaticClass()), TEXT("CooldownTag")), FString(BehaviorTreeTagAttack));
		TestEqual(TEXT("the runtime check decorator keeps its tags"),
			BehaviorTreeNodeTags(BehaviorTreeRuntimeDecorator(Tree, UBTDecorator_CheckGameplayTagsOnActor::StaticClass()), TEXT("GameplayTags")), Container);
	}

	// The document edits the tag and the container.
	if (TestTrue(TEXT("the tags are edited"), Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> CooldownRow = BehaviorTreeDecorator(Document, BehaviorTreeCooldownClass);
		const TSharedPtr<FJsonObject> CheckRow = BehaviorTreeDecorator(Document, BehaviorTreeCheckTagsClass);
		if (!CooldownRow.IsValid() || !CheckRow.IsValid())
		{
			return;
		}
		CooldownRow->GetObjectField(TEXT("Properties"))->SetStringField(TEXT("CooldownTag"), BehaviorTreeTagDodge);
		TArray<TSharedPtr<FJsonValue>> Tags;
		Tags.Add(MakeShared<FJsonValueString>(BehaviorTreeTagRooted));
		CheckRow->GetObjectField(TEXT("Properties"))->SetArrayField(TEXT("GameplayTags"), Tags);
	}, TEXT("Edit the tags")).IsSuccess()))
	{
		Tree = Target.Get<UBehaviorTree>();
		TestEqual(TEXT("the cooldown tag"),
			BehaviorTreeNodeTags(BehaviorTreeGraphDecorator(Tree, UBTDecorator_TagCooldown::StaticClass()), TEXT("CooldownTag")), FString(BehaviorTreeTagDodge));
		TestEqual(TEXT("the container"),
			BehaviorTreeNodeTags(BehaviorTreeGraphDecorator(Tree, UBTDecorator_CheckGameplayTagsOnActor::StaticClass()), TEXT("GameplayTags")),
			FString(BehaviorTreeTagRooted));
		TestEqual(TEXT("the runtime cooldown tag"),
			BehaviorTreeNodeTags(BehaviorTreeRuntimeDecorator(Tree, UBTDecorator_TagCooldown::StaticClass()), TEXT("CooldownTag")), FString(BehaviorTreeTagDodge));
		const FAssetDocumentResult Reread = Tolerant::ExtractResult(Target);
		TestEqual(TEXT("the container reads back"), Reread.IsSuccess()
			? BehaviorTreeDocumentTags(Reread.Payload, BehaviorTreeCheckTagsClass, TEXT("GameplayTags")) : FString(), FString(BehaviorTreeTagRooted));
	}
	return !HasAnyErrors();
}

#endif
