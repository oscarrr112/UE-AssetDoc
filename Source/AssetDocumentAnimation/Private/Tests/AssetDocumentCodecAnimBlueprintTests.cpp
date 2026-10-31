// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Animation Blueprint whose AnimGraph has a node with an instanced struct setting (a node of the
// AssetDocumentTests module, found by path, holding a derived struct as a Chooser Player node holds its chooser), the
// setting picked and filled in the node's Details: Extract shows {Struct, Properties} with every field, the extract
// re-applies as a no-op, a document edits one field and the held struct keeps its type and other field, and a struct
// outside the setting's base struct is refused.

#include "AssetDocumentTolerantTestSupport.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimationGraphSchema.h"
#include "BlueprintNodeBinder.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const AnimBlueprintSettingSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* const AnimBlueprintSettingNodeClass = TEXT("/Script/AssetDocumentTests.AnimGraphNode_AssetDocumentInstancedSetting");
const TCHAR* const AnimBlueprintSettingCurveStruct = TEXT("/Script/AssetDocumentTests.AssetDocumentInstancedSettingCurve");
const TCHAR* const AnimBlueprintSettingAction = TEXT("Anim:AssetDocumentInstancedSetting");
const TCHAR* const AnimBlueprintSettingKey = TEXT("Node.Setting");

// The AnimGraph of Blueprint, or null.
UEdGraph* AnimBlueprintSettingAnimGraph(UAnimBlueprint* Blueprint)
{
	const TObjectPtr<UEdGraph>* Graph = Blueprint != nullptr ? Blueprint->FunctionGraphs.FindByPredicate([](const UEdGraph* Candidate)
	{
		return Candidate != nullptr && Candidate->Schema != nullptr && Candidate->Schema->IsChildOf(UAnimationGraphSchema::StaticClass());
	}) : nullptr;
	return Graph != nullptr ? Graph->Get() : nullptr;
}

// The AnimGraph's node of Class, or null.
UAnimGraphNode_Base* AnimBlueprintSettingNode(const AssetDocumentTolerantTestSupport::FTarget& Target, const UClass* Class)
{
	UEdGraph* Graph = AnimBlueprintSettingAnimGraph(Target.Get<UAnimBlueprint>());
	for (int32 Index = 0; Graph != nullptr && Index < Graph->Nodes.Num(); ++Index)
	{
		if (Graph->Nodes[Index] != nullptr && Graph->Nodes[Index]->IsA(Class))
		{
			return Cast<UAnimGraphNode_Base>(Graph->Nodes[Index]);
		}
	}
	return nullptr;
}

// The node's Setting (its anim node's FInstancedStruct), or null.
FInstancedStruct* AnimBlueprintSettingOf(UAnimGraphNode_Base* Node)
{
	const FStructProperty* NodeProperty = Node != nullptr ? FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node")) : nullptr;
	const FStructProperty* Setting = NodeProperty != nullptr ? FindFProperty<FStructProperty>(NodeProperty->Struct, TEXT("Setting")) : nullptr;
	if (Setting == nullptr || Setting->Struct != FInstancedStruct::StaticStruct())
	{
		return nullptr;
	}
	return Setting->ContainerPtrToValuePtr<FInstancedStruct>(NodeProperty->ContainerPtrToValuePtr<void>(Node));
}

// The held struct as "<struct name> Weight=<w> Curve=<c>", or "<empty>".
FString AnimBlueprintSettingText(const FInstancedStruct* Setting)
{
	const UScriptStruct* Type = Setting != nullptr ? Setting->GetScriptStruct() : nullptr;
	if (Type == nullptr || Setting->GetMemory() == nullptr)
	{
		return TEXT("<empty>");
	}
	const FFloatProperty* Weight = FindFProperty<FFloatProperty>(Type, TEXT("Weight"));
	const FNameProperty* Curve = FindFProperty<FNameProperty>(Type, TEXT("Curve"));
	return FString::Printf(TEXT("%s Weight=%s Curve=%s"), *Type->GetName(),
		Weight != nullptr ? *FString::SanitizeFloat(Weight->GetPropertyValue_InContainer(Setting->GetMemory())) : TEXT("-"),
		Curve != nullptr ? *Curve->GetPropertyValue_InContainer(Setting->GetMemory()).ToString() : TEXT("-"));
}

// Every node object (one with an "Action") anywhere in the document.
void AnimBlueprintSettingCollectNodes(const TSharedPtr<FJsonValue>& Value, TArray<TSharedPtr<FJsonObject>>& Out)
{
	if (!Value.IsValid())
	{
		return;
	}
	if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			AnimBlueprintSettingCollectNodes(Item, Out);
		}
	}
	else if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object->HasTypedField<EJson::String>(TEXT("Action")))
		{
			Out.Add(Object);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			AnimBlueprintSettingCollectNodes(Field.Value, Out);
		}
	}
}

// The document's Node.Setting value of the instanced setting node, or null.
TSharedPtr<FJsonValue> AnimBlueprintSettingValue(const TSharedPtr<FJsonObject>& Document)
{
	TArray<TSharedPtr<FJsonObject>> Nodes;
	AnimBlueprintSettingCollectNodes(MakeShared<FJsonValueObject>(Document), Nodes);
	for (const TSharedPtr<FJsonObject>& Node : Nodes)
	{
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (Node->GetStringField(TEXT("Action")) == AnimBlueprintSettingAction && Node->TryGetObjectField(TEXT("Properties"), Properties))
		{
			return (*Properties)->TryGetField(AnimBlueprintSettingKey);
		}
	}
	return nullptr;
}

// The document's setting as AnimBlueprintSettingText spells the held struct, or why it has no such form.
FString AnimBlueprintSettingDocumentText(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonValue> Value = AnimBlueprintSettingValue(Document);
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return TEXT("<not an object>");
	}
	const TSharedPtr<FJsonObject> Setting = Value->AsObject();
	FString StructPath;
	const TSharedPtr<FJsonObject>* Fields = nullptr;
	if (!Setting->TryGetStringField(TEXT("Struct"), StructPath) || !Setting->TryGetObjectField(TEXT("Properties"), Fields)
		|| Setting->Values.Num() != 2)
	{
		return TEXT("<not {Struct, Properties}>");
	}
	double Weight = 0.0;
	FString Curve;
	if ((*Fields)->Values.Num() != 2 || !(*Fields)->TryGetNumberField(TEXT("Weight"), Weight) || !(*Fields)->TryGetStringField(TEXT("Curve"), Curve))
	{
		return TEXT("<not every field>");
	}
	return FString::Printf(TEXT("%s Weight=%s Curve=%s"), *FPackageName::ObjectPathToObjectName(StructPath),
		*FString::SanitizeFloat(static_cast<float>(Weight)), *Curve);
}

UEdGraphPin* AnimBlueprintSettingPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	for (int32 Index = 0; Node != nullptr && Index < Node->Pins.Num(); ++Index)
	{
		if (Node->Pins[Index] != nullptr && Node->Pins[Index]->Direction == Direction && !Node->Pins[Index]->bHidden)
		{
			return Node->Pins[Index];
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCodecAnimBlueprintInstancedStructSettingTest, "UEAssetDoc.Codec.AnimBlueprint.InstancedStructSetting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCodecAnimBlueprintInstancedStructSettingTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	const Tolerant::FTarget Target(TEXT("ABP_InstancedSetting"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UClass* NodeClass = FindObject<UClass>(nullptr, AnimBlueprintSettingNodeClass);
	UScriptStruct* CurveStruct = FindObject<UScriptStruct>(nullptr, AnimBlueprintSettingCurveStruct);
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, AnimBlueprintSettingSkeleton);
	if (!TestNotNull(TEXT("the test node class is loaded"), NodeClass) || !TestNotNull(TEXT("the test struct is loaded"), CurveStruct)
		|| !TestNotNull(TEXT("skeleton"), Skeleton))
	{
		return false;
	}
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(UAnimInstance::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UAnimBlueprint::StaticClass(),
		UAnimBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("animation blueprint created"), Blueprint))
	{
		return false;
	}
	Blueprint->TargetSkeleton = Skeleton;
	FAssetRegistryModule::AssetCreated(Blueprint);
	UEdGraph* AnimGraph = AnimBlueprintSettingAnimGraph(Blueprint);
	UEdGraphNode* Root = nullptr;
	for (int32 Index = 0; AnimGraph != nullptr && Index < AnimGraph->Nodes.Num(); ++Index)
	{
		Root = Root != nullptr ? Root : Cast<UAnimGraphNode_Root>(AnimGraph->Nodes[Index]);
	}
	if (!TestNotNull(TEXT("AnimGraph"), AnimGraph) || !TestNotNull(TEXT("Output Pose"), Root))
	{
		return false;
	}

	// The node from the graph's palette, into the Output Pose; its Setting picked (the derived struct) and filled in
	// the node's Details.
	const TStrongObjectPtr<UBlueprintNodeSpawner> Spawner(UBlueprintNodeSpawner::Create(NodeClass));
	UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(Spawner->Invoke(AnimGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(-300.0, 0.0)));
	FInstancedStruct* Setting = AnimBlueprintSettingOf(Node);
	if (!TestNotNull(TEXT("node placed"), Node) || !TestNotNull(TEXT("its Setting"), Setting))
	{
		return false;
	}
	Node->Modify();
	Setting->InitializeAs(CurveStruct);
	const FFloatProperty* WeightField = FindFProperty<FFloatProperty>(CurveStruct, TEXT("Weight"));
	const FNameProperty* CurveField = FindFProperty<FNameProperty>(CurveStruct, TEXT("Curve"));
	if (!TestNotNull(TEXT("Weight field"), WeightField) || !TestNotNull(TEXT("Curve field"), CurveField))
	{
		return false;
	}
	WeightField->SetPropertyValue_InContainer(Setting->GetMutableMemory(), 0.25f);
	CurveField->SetPropertyValue_InContainer(Setting->GetMutableMemory(), FName(TEXT("Lean")));
	FPropertyChangedEvent Event(FindFProperty<FProperty>(Node->GetClass(), TEXT("Node")), EPropertyChangeType::ValueSet);
	Node->PostEditChangeProperty(Event);
	UEdGraphPin* NodeOutput = AnimBlueprintSettingPin(Node, EGPD_Output);
	UEdGraphPin* RootInput = AnimBlueprintSettingPin(Root, EGPD_Input);
	TestTrue(TEXT("node linked to the Output Pose"), NodeOutput != nullptr && RootInput != nullptr
		&& AnimGraph->GetSchema()->TryCreateConnection(NodeOutput, RootInput));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("fixture compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const FString Held = FString::Printf(TEXT("%s Weight=0.25 Curve=Lean"), *CurveStruct->GetName());
	TestEqual(TEXT("the node holds the setting"), AnimBlueprintSettingText(AnimBlueprintSettingOf(AnimBlueprintSettingNode(Target, NodeClass))), Held);

	// Extract shows the held struct and every field; the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Instanced setting"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Node.Setting reads as {Struct, Properties}"), AnimBlueprintSettingDocumentText(Extracted), Held);
	{
		const TSharedPtr<FJsonValue> ExtractedSetting = AnimBlueprintSettingValue(Extracted);
		FString ExtractedPath;
		TestTrue(TEXT("Struct is the held struct's path"), ExtractedSetting.IsValid() && ExtractedSetting->Type == EJson::Object
			&& ExtractedSetting->AsObject()->TryGetStringField(TEXT("Struct"), ExtractedPath) && ExtractedPath == CurveStruct->GetPathName());
	}

	// One field edited: the held struct keeps its type and its other field.
	if (TestTrue(TEXT("a field is edited"), Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Value = AnimBlueprintSettingValue(Document);
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		if (Value.IsValid() && Value->Type == EJson::Object && Value->AsObject()->TryGetObjectField(TEXT("Properties"), Fields))
		{
			(*Fields)->SetNumberField(TEXT("Weight"), 0.75);
		}
	}, TEXT("Edit a field")).IsSuccess()))
	{
		const FString Edited = FString::Printf(TEXT("%s Weight=0.75 Curve=Lean"), *CurveStruct->GetName());
		TestEqual(TEXT("the node holds the edited setting"),
			AnimBlueprintSettingText(AnimBlueprintSettingOf(AnimBlueprintSettingNode(Target, NodeClass))), Edited);
		const FAssetDocumentResult Reread = Tolerant::ExtractResult(Target);
		TestEqual(TEXT("the edited setting reads back"), Reread.IsSuccess() ? AnimBlueprintSettingDocumentText(Reread.Payload) : FString(), Edited);
	}

	// A struct the setting's base struct does not allow.
	const FString BeforeRefusal = AnimBlueprintSettingText(AnimBlueprintSettingOf(AnimBlueprintSettingNode(Target, NodeClass)));
	const FAssetDocumentResult Refused = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Value = AnimBlueprintSettingValue(Document);
		if (Value.IsValid() && Value->Type == EJson::Object)
		{
			Value->AsObject()->SetStringField(TEXT("Struct"), TEXT("/Script/CoreUObject.Vector"));
			Value->AsObject()->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
		}
	}, TEXT("Struct outside the base struct"));
	TestFalse(TEXT("a struct outside the base struct is refused"), Refused.IsSuccess());
	TestEqual(TEXT("the refused struct leaves the setting"), AnimBlueprintSettingText(AnimBlueprintSettingOf(AnimBlueprintSettingNode(Target, NodeClass))),
		BeforeRefusal);
	return !HasAnyErrors();
}

#endif
