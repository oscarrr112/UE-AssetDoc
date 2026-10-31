// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Blueprint holding values too large to read in one member query page (a component's tag list and
// the class defaults' Tags with thousands of entries): they read as markers with warnings, the extract re-applies as
// a no-op, other edits apply and keep them (left or omitted), and an edited marker is refused.

#include "AssetDocumentTolerantTestSupport.h"

#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantBlueprintMemberTests
{
// Over the member query's 3000-node limit for one value.
constexpr int32 ManyTags = 3500;

TArray<FName> MakeTags(const TCHAR* Prefix)
{
	TArray<FName> Tags;
	for (int32 Index = 0; Index < ManyTags; ++Index)
	{
		Tags.Add(FName(*FString::Printf(TEXT("%s%d"), Prefix, Index)));
	}
	return Tags;
}

// The index of the component named Name in the document's Body.Components (INDEX_NONE if none).
int32 ComponentIndex(const TSharedPtr<FJsonObject>& Document, const FString& Name)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(TEXT("Components"), Components))
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < Components->Num(); ++Index)
	{
		FString Found;
		if ((*Components)[Index]->Type == EJson::Object && (*Components)[Index]->AsObject()->TryGetStringField(TEXT("Name"), Found) && Found == Name)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

USceneComponent* KitTemplate(UBlueprint* Blueprint)
{
	USCS_Node* Node = Blueprint != nullptr && Blueprint->SimpleConstructionScript != nullptr
		? Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("Kit")) : nullptr;
	return Node != nullptr ? Cast<USceneComponent>(Node->ComponentTemplate.Get()) : nullptr;
}

const AActor* ClassDefaults(UBlueprint* Blueprint)
{
	return Blueprint != nullptr && Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass->GetDefaultObject<AActor>() : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBlueprintMembersTest, "UEAssetDoc.Tolerant.BlueprintMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBlueprintMembersTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBlueprintMemberTests;
	const FTarget Target(TEXT("BP_LevelKit"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Target.PackageName),
		FName(*Target.AssetName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("blueprint created"), Blueprint) || !TestNotNull(TEXT("construction script"), Blueprint->SimpleConstructionScript.Get()))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	// Components panel > Add > Scene Component under the default scene root, with thousands of Component Tags.
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript.Get();
	USCS_Node* Kit = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("Kit"));
	if (USCS_Node* Root = SCS->GetDefaultSceneRootNode())
	{
		Root->AddChildNode(Kit);
	}
	else
	{
		SCS->AddNode(Kit);
	}
	CastChecked<USceneComponent>(Kit->ComponentTemplate.Get())->ComponentTags = MakeTags(TEXT("Piece"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	// Class Defaults > Actor > Tags, thousands of them.
	AActor* Defaults = Blueprint->GeneratedClass->GetDefaultObject<AActor>();
	Defaults->Tags = MakeTags(TEXT("Cell"));
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FAssetDocumentResult First = ExtractResult(Target);
	const int32 KitIndex = ComponentIndex(First.Payload, TEXT("Kit"));
	if (!TestTrue(TEXT("the component is extracted"), First.IsSuccess() && KitIndex != INDEX_NONE))
	{
		LogResult(*this, TEXT("First extract"), First);
		return false;
	}
	const FString KitTags = FString::Printf(TEXT("/Body/Components/%d/Properties/ComponentTags"), KitIndex);
	FExpected Expected;
	Expected.Warnings = {
		{TEXT("UBlueprint.Unrepresented"), KitTags},
		{TEXT("UBlueprint.Unrepresented"), TEXT("/Body/ClassDefaults/Tags")}};
	Expected.Markers = {KitTags, TEXT("/Body/ClassDefaults/Tags")};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Oversized values"));
	if (!Extracted.IsValid())
	{
		return false;
	}

	// Other edits apply; the component's marked tags are omitted and the class defaults' marked Tags left as read:
	// both are kept.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [KitIndex](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
		Body->GetObjectField(TEXT("ClassDefaults"))->SetNumberField(TEXT("InitialLifeSpan"), 5.0);
		const TSharedPtr<FJsonObject> Component = Body->GetArrayField(TEXT("Components"))[KitIndex]->AsObject();
		const TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetBoolField(TEXT("bHiddenInGame"), true);
		Component->SetObjectField(TEXT("Properties"), Properties);
	}, TEXT("Edit"));
	if (TestTrue(TEXT("another edit applies"), Edited.IsSuccess()))
	{
		Blueprint = Target.Get<UBlueprint>();
		const USceneComponent* Template = KitTemplate(Blueprint);
		const AActor* After = ClassDefaults(Blueprint);
		TestTrue(TEXT("the edit is written"), Template != nullptr && Template->bHiddenInGame && After != nullptr && After->InitialLifeSpan == 5.0f);
		TestTrue(TEXT("the omitted component tags are kept"), Template != nullptr && Template->ComponentTags.Num() == ManyTags);
		TestTrue(TEXT("the class defaults' tags are kept"), After != nullptr && After->Tags.Num() == ManyTags);
		TestStillShows(*this, Target, TEXT("UBlueprint.Unrepresented"), KitTags, TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("UBlueprint.Unrepresented"), TEXT("/Body/ClassDefaults/Tags"), TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("ClassDefaults"))->GetObjectField(TEXT("Tags"))
			->SetStringField(TEXT("Unsupported"), TEXT("edited"));
	}, TEXT("Edited marker")), TEXT("UBlueprint.UnrepresentedValue"), TEXT("/Body/ClassDefaults/Tags"), TEXT("Edited marker"));
	TestTrue(TEXT("tags still kept after the refusal"), ClassDefaults(Target.Get<UBlueprint>()) != nullptr
		&& ClassDefaults(Target.Get<UBlueprint>())->Tags.Num() == ManyTags);
	return !HasAnyErrors();
}

#endif
