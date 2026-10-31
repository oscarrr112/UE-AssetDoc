// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Sound Cue holding a project sound node whose instanced settings object was made with the Details
// panel's class picker. The object reads as a marker with a warning, the extract re-applies as a no-op, an edit of the
// node's other property keeps the object, and an edited marker is refused. The node class is a test type of the
// AssetDocumentTests module (UAssetDocumentSoundCueTestNode), found by path: the automation framework loads that
// module before it lists the tests.

#include "AssetDocumentTolerantTestSupport.h"
#include "SoundCue/AssetDocumentSoundCue.h"

#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Factories/SoundCueFactoryNew.h"
#include "IAssetTools.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantSoundCueTestsLocal
{
const TCHAR* const NodeClassPath = TEXT("/Script/AssetDocumentTests.AssetDocumentSoundCueTestNode");

TSharedPtr<FJsonObject> NodeProperties(const TSharedPtr<FJsonObject>& Document, const FString& Key)
{
	const TSharedPtr<FJsonObject> Node = Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Nodes"))->GetObjectField(Key);
	if (!Node->HasField(TEXT("Properties")))
	{
		Node->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	}
	return Node->GetObjectField(TEXT("Properties"));
}

UObject* SettingsOf(const USoundNode* Node)
{
	const FObjectProperty* Property = Node != nullptr ? CastField<FObjectProperty>(Node->GetClass()->FindPropertyByName(TEXT("Settings"))) : nullptr;
	return Property != nullptr ? Property->GetObjectPropertyValue_InContainer(Node) : nullptr;
}

USoundNode* FindNode(const USoundCue* Cue, const FString& Key)
{
	if (Cue != nullptr)
	{
		for (USoundNode* Node : Cue->AllNodes)
		{
			if (Node != nullptr && Node->GetName() == Key)
			{
				return Node;
			}
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantSoundCueTest, "UEAssetDoc.Tolerant.SoundCue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantSoundCueTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantSoundCueTestsLocal;
	namespace SC = UE::AssetDocument::Graph::Audio::SoundCue;
	const FTarget Target(TEXT("SC_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UClass* NodeClass = FindObject<UClass>(nullptr, NodeClassPath);
	if (!TestNotNull(TEXT("the test sound node class is loaded"), NodeClass))
	{
		return false;
	}

	// Content Browser: Add > Sounds > Sound Cue.
	USoundCueFactoryNew* Factory = NewObject<USoundCueFactoryNew>();
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	USoundCue* Cue = Cast<USoundCue>(AssetTools.CreateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName),
		USoundCue::StaticClass(), Factory));
	if (!TestTrue(TEXT("cue created with its graph"), Cue != nullptr && Cue->SoundCueGraph != nullptr))
	{
		return false;
	}

	// Palette: place the node, drag its output to the Output node.
	USoundCueGraphNode* GraphNode = Cast<USoundCueGraphNode>(SC::PerformPaletteAction(*Cue->SoundCueGraph, NodeClass, FVector2f(-300.f, 0.f)));
	if (!TestTrue(TEXT("palette places the node"), GraphNode != nullptr && GraphNode->SoundNode != nullptr))
	{
		return false;
	}
	USoundNode* Node = GraphNode->SoundNode;
	const FString Key = Node->GetName();
	for (UEdGraphNode* Candidate : Cue->SoundCueGraph->Nodes)
	{
		if (USoundCueGraphNode_Root* Root = Cast<USoundCueGraphNode_Root>(Candidate))
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentTolerantSoundCueTests", "LinkPins", "Link Pins"));
			TestTrue(TEXT("node linked to the Output node"),
				Cue->SoundCueGraph->GetSchema()->TryCreateConnection(GraphNode->GetOutputPin(), Root->GetInputPin(0)));
		}
	}

	// Details panel: the class picker of the instanced Settings property makes the object inside the node.
	FObjectProperty* SettingsProperty = CastField<FObjectProperty>(NodeClass->FindPropertyByName(TEXT("Settings")));
	FProperty* RangeProperty = SettingsProperty != nullptr ? SettingsProperty->PropertyClass->FindPropertyByName(TEXT("Range")) : nullptr;
	if (!TestTrue(TEXT("Settings and Range properties"), SettingsProperty != nullptr && RangeProperty != nullptr))
	{
		return false;
	}
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentTolerantSoundCueTests", "EditDetail", "Edit Property"));
		Node->Modify();
		Node->PreEditChange(SettingsProperty);
		UObject* Settings = NewObject<UObject>(Node, SettingsProperty->PropertyClass, NAME_None, RF_Transactional);
		CastField<FFloatProperty>(RangeProperty)->SetPropertyValue_InContainer(Settings, 3.f);
		SettingsProperty->SetObjectPropertyValue_InContainer(Node, Settings);
		FPropertyChangedEvent Event(SettingsProperty, EPropertyChangeType::ValueSet);
		Node->PostEditChangeProperty(Event);
	}
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString Marker = FString::Printf(TEXT("/Body/Nodes/%s/Properties/Settings"), *Key);
	FExpected Expected;
	Expected.Warnings = {{TEXT("SoundCue.Unrepresented"), Marker}};
	Expected.Markers = {Marker};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Sound Cue")).IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("another property of the node changes"), ApplyEdited(*this, Target, [&Key](const TSharedPtr<FJsonObject>& Document)
	{
		NodeProperties(Document, Key)->SetNumberField(TEXT("Gain"), 0.5);
	}, TEXT("Edit")).IsSuccess()))
	{
		const USoundNode* Edited = FindNode(Target.Get<USoundCue>(), Key);
		const FFloatProperty* Gain = Edited != nullptr ? CastField<FFloatProperty>(Edited->GetClass()->FindPropertyByName(TEXT("Gain"))) : nullptr;
		TestTrue(TEXT("Gain written"), Gain != nullptr && Gain->GetPropertyValue_InContainer(Edited) == 0.5f);
		const UObject* Kept = SettingsOf(Edited);
		TestTrue(TEXT("the settings object kept"), Kept != nullptr && Kept->GetOuter() == Edited
			&& CastField<FFloatProperty>(RangeProperty)->GetPropertyValue_InContainer(Kept) == 3.f);
		TestStillShows(*this, Target, TEXT("SoundCue.Unrepresented"), Marker, TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [&Key](const TSharedPtr<FJsonObject>& Document)
	{
		NodeProperties(Document, Key)->GetObjectField(TEXT("Settings"))->SetStringField(TEXT("Text"), TEXT("None"));
	}, TEXT("Edited marker")), TEXT("SoundCue.UnrepresentedValue"), Marker, TEXT("Edited marker"));
	return !HasAnyErrors();
}

#endif
