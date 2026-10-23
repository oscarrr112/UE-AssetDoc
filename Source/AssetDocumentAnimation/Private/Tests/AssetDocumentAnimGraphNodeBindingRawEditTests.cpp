// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Behaviour test of the raw-edit file AnimBlueprint/AssetDocumentAnimGraphNodeBindingRawEdits:
// pin property bindings of an anim graph node are written the way the details-panel
// binding widget writes them (exposed pin, no links, compiled binding) and read back.

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"
#include "AnimBlueprint/AssetDocumentAnimGraphNodeBindingRawEdits.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimationGraphSchema.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAnimGraphNodeBindingRawEditTests
{
using namespace UE::AssetDocument::AnimBlueprint;

const TCHAR* Skeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");

FValue Binding(const TCHAR* Pin, const TCHAR* Path, bool bOnlyUpdateWhenActive = false)
{
	FValue Entry = MakeObjectValue();
	SetField(Entry, TEXT("Path"), MakeStringValue(Path));
	if (bOnlyUpdateWhenActive)
	{
		SetField(Entry, TEXT("OnlyUpdateWhenActive"), MakeBoolValue(true));
	}
	FValue Out = MakeObjectValue();
	SetField(Out, Pin, MoveTemp(Entry));
	return Out;
}

bool Compiles(UBlueprint& Blueprint)
{
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	return Blueprint.Status == BS_UpToDate || Blueprint.Status == BS_UpToDateWithWarnings;
}
} // namespace AssetDocumentAnimGraphNodeBindingRawEditTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimGraphNodeBindingRawEditTest,
	"UEAssetDoc.AnimBlueprint.Adapters.NodeBindingRawEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimGraphNodeBindingRawEditTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimGraphNodeBindingRawEditTests;
	using namespace UE::AssetDocument::AnimBlueprint::BindingRawEdits;

	USkeleton* TargetSkeleton = LoadObject<USkeleton>(nullptr, Skeleton);
	if (!TestNotNull(TEXT("fixture skeleton loads"), TargetSkeleton))
	{
		return false;
	}
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/AssetDocumentBindingRawEdit_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UAnimBlueprint* Blueprint = CastChecked<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), Package, TEXT("ABP_BindingRawEdit"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
	ON_SCOPE_EXIT
	{
		Blueprint->ClearFlags(RF_Public | RF_Standalone);
		Blueprint->MarkAsGarbage();
		Package->MarkAsGarbage();
	};
	Blueprint->TargetSkeleton = TargetSkeleton;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Speed"), FEdGraphPinType(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Float, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Label"), FEdGraphPinType(UEdGraphSchema_K2::PC_String, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
	TestTrue(TEXT("fixture compiles"), Compiles(*Blueprint));

	const TObjectPtr<UEdGraph>* AnimGraph = Blueprint->FunctionGraphs.FindByPredicate(
		[](const UEdGraph* Graph) { return Graph != nullptr && Graph->Schema != nullptr && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()); });
	if (!TestNotNull(TEXT("fixture has an AnimGraph"), AnimGraph))
	{
		return false;
	}
	const TStrongObjectPtr<UBlueprintNodeSpawner> Spawner(UBlueprintNodeSpawner::Create(UAnimGraphNode_TwoWayBlend::StaticClass()));
	UAnimGraphNode_TwoWayBlend* Node = Cast<UAnimGraphNode_TwoWayBlend>(
		Spawner->Invoke(AnimGraph->Get(), IBlueprintNodeBinder::FBindingSet(), FVector2D::ZeroVector));
	if (!TestNotNull(TEXT("Two Way Blend spawns"), Node))
	{
		return false;
	}
	TestEqual(TEXT("a fresh node has no bindings"), QueryNodeBindings(*Node).Kind, EAssetDocumentRuntimeValueKind::Null);

	// Bind Alpha to Speed: the pin is exposed, unlinked and the binding reads back.
	FAssetDocumentGraphError Error;
	TestTrue(TEXT("binding Alpha to Speed succeeds"), SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("Speed")), Error));
	const FValue Bound = QueryNodeBindings(*Node);
	const FValue* Alpha = FindValueField(Bound, TEXT("Alpha"));
	const FValue* Path = Alpha != nullptr ? FindValueField(*Alpha, TEXT("Path")) : nullptr;
	TestTrue(TEXT("Alpha reads back bound to Speed"), Path != nullptr && Path->StringValue == TEXT("Speed"));
	TestNull(TEXT("OnlyUpdateWhenActive is omitted when false"), Alpha != nullptr ? FindValueField(*Alpha, TEXT("OnlyUpdateWhenActive")) : nullptr);
	const UEdGraphPin* AlphaPin = Node->FindPin(TEXT("Alpha"));
	TestTrue(TEXT("bound pin is exposed without links"), AlphaPin != nullptr && AlphaPin->LinkedTo.IsEmpty());
	TestTrue(TEXT("bound Animation Blueprint compiles"), Compiles(*Blueprint));

	// OnlyUpdateWhenActive round trip.
	TestTrue(TEXT("rebinding with OnlyUpdateWhenActive succeeds"), SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("Speed"), true), Error));
	const FValue ReboundValue = QueryNodeBindings(*Node);
	const FValue* Rebound = FindValueField(ReboundValue, TEXT("Alpha"));
	const FValue* Only = Rebound != nullptr ? FindValueField(*Rebound, TEXT("OnlyUpdateWhenActive")) : nullptr;
	TestTrue(TEXT("OnlyUpdateWhenActive reads back"), Only != nullptr && Only->BooleanValue);

	// Rejected edits leave the bindings untouched.
	FAssetDocumentGraphError Unknown;
	TestFalse(TEXT("unknown pin property is rejected"), SetNodeBindings(*Node, Binding(TEXT("NoSuchPin"), TEXT("Speed")), Unknown));
	TestEqual(TEXT("unknown pin property error code"), Unknown.Code, FString(TEXT("Graph.InvalidArgument")));
	FAssetDocumentGraphError Missing;
	TestFalse(TEXT("unknown variable is rejected"), SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("NoSuchVariable")), Missing));
	FAssetDocumentGraphError Incompatible;
	TestFalse(TEXT("incompatible variable type is rejected"), SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("Label")), Incompatible));
	FAssetDocumentGraphError Shape;
	TestFalse(TEXT("non-object value is rejected"), SetNodeBindings(*Node, MakeStringValue(TEXT("Speed")), Shape));
	const FValue KeptValue = QueryNodeBindings(*Node);
	const FValue* Kept = FindValueField(KeptValue, TEXT("Alpha"));
	const FValue* KeptPath = Kept != nullptr ? FindValueField(*Kept, TEXT("Path")) : nullptr;
	TestTrue(TEXT("rejected edits keep the binding"), KeptPath != nullptr && KeptPath->StringValue == TEXT("Speed"));

	// A pure function with no parameters and a compatible return value binds as a function
	// (UAnimInstance::GetDeltaSeconds); an incompatible return value is rejected.
	TestTrue(TEXT("binding Alpha to a pure function succeeds"), SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("GetDeltaSeconds")), Error));
	const FValue FunctionValue = QueryNodeBindings(*Node);
	const FValue* Function = FindValueField(FunctionValue, TEXT("Alpha"));
	const FValue* FunctionPath = Function != nullptr ? FindValueField(*Function, TEXT("Path")) : nullptr;
	TestTrue(TEXT("function binding reads back"), FunctionPath != nullptr && FunctionPath->StringValue == TEXT("GetDeltaSeconds"));
	TestTrue(TEXT("function-bound Animation Blueprint compiles"), Compiles(*Blueprint));
	FAssetDocumentGraphError WrongReturn;
	TestFalse(TEXT("function with an incompatible return value is rejected"),
		SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("GetOwningActor")), WrongReturn));
	TestTrue(TEXT("back to the property binding succeeds"), SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("Speed")), Error));
	TestTrue(TEXT("property-bound Animation Blueprint compiles"), Compiles(*Blueprint));

	// {} removes every binding.
	TestTrue(TEXT("empty object clears bindings"), SetNodeBindings(*Node, MakeObjectValue(), Error));
	TestEqual(TEXT("no bindings remain"), QueryNodeBindings(*Node).Kind, EAssetDocumentRuntimeValueKind::Null);
	TestTrue(TEXT("unbound Animation Blueprint compiles"), Compiles(*Blueprint));
	return true;
}

#endif
