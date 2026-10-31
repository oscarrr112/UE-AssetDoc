// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Behaviour test of the raw-edit file AnimBlueprint/AssetDocumentAnimGraphNodeBindingRawEdits:
// pin property bindings of an anim graph node are written the way the details-panel
// binding widget writes them (exposed pin, no links, compiled binding) and read back.
// The numbered pin node is a test type of AssetDocumentTests (AssetDocumentAnimBlueprintNumberedPinTestTypes.h).

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"
#include "AnimBlueprint/AssetDocumentAnimGraphNodeBindingRawEdits.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimGraphNode_BlendListByInt.h"
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
const TCHAR* const NumberedPinNodeClass = TEXT("/Script/AssetDocumentTests.AnimGraphNode_AssetDocumentNumberedPin");

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

FValue BindingAt(const TCHAR* Pin, const TCHAR* Path, const TCHAR* ContextId)
{
	FValue Entry = MakeObjectValue();
	SetField(Entry, TEXT("Path"), MakeStringValue(Path));
	SetField(Entry, TEXT("ContextId"), MakeStringValue(ContextId));
	FValue Out = MakeObjectValue();
	SetField(Out, Pin, MoveTemp(Entry));
	return Out;
}

FString ContextOf(const UAnimGraphNode_Base& Node, const TCHAR* Pin)
{
	const FValue Bindings = UE::AssetDocument::AnimBlueprint::BindingRawEdits::QueryNodeBindings(Node);
	const FValue* Entry = FindValueField(Bindings, Pin);
	const FValue* Context = Entry != nullptr ? FindValueField(*Entry, TEXT("ContextId")) : nullptr;
	return Context != nullptr ? Context->StringValue : FString();
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

	// The binding's call site: set, kept by an entry without ContextId, back to Automatic with "".
	TestTrue(TEXT("binding with a call site succeeds"),
		SetNodeBindings(*Node, BindingAt(TEXT("Alpha"), TEXT("Speed"), TEXT("Batched_GameThreadPreEventGraph")), Error));
	TestEqual(TEXT("the call site reads back"), ContextOf(*Node, TEXT("Alpha")), FString(TEXT("Batched_GameThreadPreEventGraph")));
	TestTrue(TEXT("an entry without ContextId applies"), SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("Speed")), Error));
	TestEqual(TEXT("an entry without ContextId keeps the call site"), ContextOf(*Node, TEXT("Alpha")),
		FString(TEXT("Batched_GameThreadPreEventGraph")));
	TestTrue(TEXT("call-site-bound Animation Blueprint compiles"), Compiles(*Blueprint));
	FAssetDocumentGraphError BadSite;
	TestFalse(TEXT("an unknown call site is rejected"), SetNodeBindings(*Node, BindingAt(TEXT("Alpha"), TEXT("Speed"), TEXT("Nowhere")), BadSite));
	TestTrue(TEXT("the unknown call site error lists the call sites"), BadSite.Message.Contains(TEXT("UnBatched_ThreadSafe")));
	TestTrue(TEXT("ContextId \"\" applies"), SetNodeBindings(*Node, BindingAt(TEXT("Alpha"), TEXT("Speed"), TEXT("")), Error));
	TestEqual(TEXT("Automatic is omitted"), ContextOf(*Node, TEXT("Alpha")), FString());

	// Bindings as the node has them (an entry without ContextId keeps any call site) change nothing: the node is not
	// reconstructed and the Blueprint is not dirtied.
	TestTrue(TEXT("binding with a call site again"),
		SetNodeBindings(*Node, BindingAt(TEXT("Alpha"), TEXT("Speed"), TEXT("UnBatched_ThreadSafe")), Error));
	Package->ClearDirtyFlag();
	const TArray<UEdGraphPin*> PinsBefore = Node->Pins;
	TestTrue(TEXT("unchanged bindings apply"), SetNodeBindings(*Node, Binding(TEXT("Alpha"), TEXT("Speed")), Error));
	TestTrue(TEXT("unchanged bindings with their call site apply"),
		SetNodeBindings(*Node, BindingAt(TEXT("Alpha"), TEXT("Speed"), TEXT("UnBatched_ThreadSafe")), Error));
	TestFalse(TEXT("unchanged bindings leave the package clean"), Package->IsDirty());
	TestTrue(TEXT("unchanged bindings keep the node's pins"), Node->Pins == PinsBefore);
	TestEqual(TEXT("and the call site"), ContextOf(*Node, TEXT("Alpha")), FString(TEXT("UnBatched_ThreadSafe")));

	// {} removes every binding.
	TestTrue(TEXT("empty object clears bindings"), SetNodeBindings(*Node, MakeObjectValue(), Error));
	TestEqual(TEXT("no bindings remain"), QueryNodeBindings(*Node).Kind, EAssetDocumentRuntimeValueKind::Null);
	TestTrue(TEXT("unbound Animation Blueprint compiles"), Compiles(*Blueprint));

	// An array element's pin (Blend List by Int's "BlendTime_0") binds that element; the whole array's binding and an
	// element's replace each other.
	const TStrongObjectPtr<UBlueprintNodeSpawner> ListSpawner(UBlueprintNodeSpawner::Create(UAnimGraphNode_BlendListByInt::StaticClass()));
	UAnimGraphNode_BlendListByInt* List = Cast<UAnimGraphNode_BlendListByInt>(
		ListSpawner->Invoke(AnimGraph->Get(), IBlueprintNodeBinder::FBindingSet(), FVector2D(0.0, 400.0)));
	if (!TestNotNull(TEXT("Blend List by Int spawns"), List))
	{
		return false;
	}
	const FName FirstTime(TEXT("BlendTime"), 1);
	TestTrue(TEXT("the first element's pin exists"), List->FindPin(FirstTime) != nullptr);
	TestTrue(TEXT("binding an array element succeeds"), SetNodeBindings(*List, Binding(*FirstTime.ToString(), TEXT("Speed")), Error));
	const FValue ElementValue = QueryNodeBindings(*List);
	const FValue* Element = FindValueField(ElementValue, *FirstTime.ToString());
	const FValue* ElementPath = Element != nullptr ? FindValueField(*Element, TEXT("Path")) : nullptr;
	TestTrue(TEXT("the element binding reads back by the element's pin"), ElementPath != nullptr && ElementPath->StringValue == TEXT("Speed"));
	TestTrue(TEXT("element-bound Animation Blueprint compiles"), Compiles(*Blueprint));
	FAssetDocumentGraphError NoElement;
	TestFalse(TEXT("an element without a pin is rejected"),
		SetNodeBindings(*List, Binding(*FName(TEXT("BlendTime"), 9).ToString(), TEXT("Speed")), NoElement));
	TestTrue(TEXT("clearing the element binding succeeds"), SetNodeBindings(*List, MakeObjectValue(), Error));
	TestEqual(TEXT("no element binding remains"), QueryNodeBindings(*List).Kind, EAssetDocumentRuntimeValueKind::Null);

	// A plain property named like an array element ("Weight_1", next to "Weight") binds by its whole name.
	UClass* NumberedClass = LoadObject<UClass>(nullptr, NumberedPinNodeClass);
	if (!TestNotNull(TEXT("numbered pin test node class"), NumberedClass))
	{
		return false;
	}
	const TStrongObjectPtr<UBlueprintNodeSpawner> NumberedSpawner(UBlueprintNodeSpawner::Create(NumberedClass));
	UAnimGraphNode_Base* Numbered = Cast<UAnimGraphNode_Base>(
		NumberedSpawner->Invoke(AnimGraph->Get(), IBlueprintNodeBinder::FBindingSet(), FVector2D(0.0, 800.0)));
	if (!TestNotNull(TEXT("numbered pin node spawns"), Numbered))
	{
		return false;
	}
	TestTrue(TEXT("binding Weight_1 succeeds"), SetNodeBindings(*Numbered, Binding(TEXT("Weight_1"), TEXT("Speed")), Error));
	const FValue NumberedValue = QueryNodeBindings(*Numbered);
	TestTrue(TEXT("Weight_1 reads back by its name"), FindValueField(NumberedValue, TEXT("Weight_1")) != nullptr);
	TestTrue(TEXT("Weight stays unbound"), FindValueField(NumberedValue, TEXT("Weight")) == nullptr);
	TestTrue(TEXT("Weight_1's pin is shown"), Numbered->FindPin(TEXT("Weight_1")) != nullptr);
	TestTrue(TEXT("Weight's pin stays hidden"), Numbered->FindPin(TEXT("Weight")) == nullptr);
	FAssetDocumentGraphError NoNumbered;
	TestFalse(TEXT("an unknown numbered name is refused"),
		SetNodeBindings(*Numbered, Binding(TEXT("Weight_2"), TEXT("Speed")), NoNumbered));
	return true;
}

#endif
