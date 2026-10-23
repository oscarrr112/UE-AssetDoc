// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Behaviour test of the raw-edit file AnimBlueprint/AssetDocumentAnimBlendSpaceGraphRawEdits:
// the blend space of a Blend Space Graph node is edited the way the Blend Space Graph editor
// tab edits it (axis settings through PreEditChange/PostEditChangeProperty, samples added /
// moved / removed together with their sample graphs) and reads back; rejected edits change
// nothing; the Animation Blueprint compiles.

#include "AnimBlueprint/AssetDocumentAnimBlendSpaceGraphRawEdits.h"
#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"

#include "AnimGraphNode_BlendSpaceGraph.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/BlendSpace.h"
#include "AnimationGraphSchema.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimBlendSpaceGraphRawEditTests
{
using namespace UE::AssetDocument::AnimBlueprint;

const TCHAR* Skeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");

FValue Sample(const TCHAR* Name, double X, TOptional<double> Y = 0.0)
{
	FValue Row = MakeObjectValue();
	SetField(Row, TEXT("Name"), MakeStringValue(Name));
	SetField(Row, TEXT("X"), MakeNumberValue(X));
	if (Y.IsSet())
	{
		SetField(Row, TEXT("Y"), MakeNumberValue(Y.GetValue()));
	}
	return Row;
}

TArray<FString> GraphNames(const UAnimGraphNode_BlendSpaceGraphBase& Node)
{
	TArray<FString> Names;
	for (const UEdGraph* Graph : BlendSpaceRawEdits::GetSampleGraphs(Node))
	{
		Names.Add(Graph != nullptr ? Graph->GetName() : FString());
	}
	return Names;
}

bool Compiles(UBlueprint& Blueprint)
{
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	return Blueprint.Status == BS_UpToDate || Blueprint.Status == BS_UpToDateWithWarnings;
}
} // namespace AssetDocumentAnimBlendSpaceGraphRawEditTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimBlendSpaceGraphRawEditTest,
	"UEAssetDoc.AnimBlueprint.Adapters.BlendSpaceGraphRawEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlendSpaceGraphRawEditTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimBlendSpaceGraphRawEditTests;
	using namespace UE::AssetDocument::AnimBlueprint::BlendSpaceRawEdits;

	USkeleton* TargetSkeleton = LoadObject<USkeleton>(nullptr, Skeleton);
	if (!TestNotNull(TEXT("fixture skeleton loads"), TargetSkeleton))
	{
		return false;
	}
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/AssetDocumentBlendSpaceRawEdit_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UAnimBlueprint* Blueprint = CastChecked<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), Package, TEXT("ABP_BlendSpaceRawEdit"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
	ON_SCOPE_EXIT
	{
		Blueprint->ClearFlags(RF_Public | RF_Standalone);
		Blueprint->MarkAsGarbage();
		Package->MarkAsGarbage();
	};
	Blueprint->TargetSkeleton = TargetSkeleton;
	const TObjectPtr<UEdGraph>* AnimGraph = Blueprint->FunctionGraphs.FindByPredicate(
		[](const UEdGraph* Graph) { return Graph != nullptr && Graph->Schema != nullptr && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()); });
	if (!TestNotNull(TEXT("fixture has an AnimGraph"), AnimGraph))
	{
		return false;
	}
	// The palette's "Blendspace" entry (UAnimGraphNode_BlendSpaceGraph::GetMenuActions, class variant).
	UBlueprintNodeSpawner* Spawner = nullptr;
	if (const FBlueprintActionDatabase::FActionList* Actions = FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(UBlendSpace::StaticClass())))
	{
		for (UBlueprintNodeSpawner* Candidate : *Actions)
		{
			Spawner = Candidate != nullptr && Candidate->NodeClass == UAnimGraphNode_BlendSpaceGraph::StaticClass() ? Candidate : Spawner;
		}
	}
	if (!TestNotNull(TEXT("the Blendspace palette action exists"), Spawner))
	{
		return false;
	}
	UAnimGraphNode_BlendSpaceGraph* Node = Cast<UAnimGraphNode_BlendSpaceGraph>(
		Spawner->Invoke(AnimGraph->Get(), IBlueprintNodeBinder::FBindingSet(), FVector2D::ZeroVector));
	if (!TestNotNull(TEXT("Blend Space Graph node spawns"), Node) || !TestNotNull(TEXT("node owns a blend space"), GetBlendSpace(*Node)))
	{
		return false;
	}
	UBlendSpace* BlendSpace = GetBlendSpace(*Node);

	// Axis setting: X range 0..200.
	FAssetDocumentGraphError Error;
	TestTrue(TEXT("X Max is set"), SetBlendSpaceSetting(*Node, TEXT("BlendSpace.BlendParameters.X.Max"), MakeNumberValue(200.0), Error));
	TestEqual(TEXT("X Max reads back on the blend space"), BlendSpace->GetBlendParameter(0).Max, 200.f);
	FValue Query = MakeObjectValue();
	QueryBlendSpace(*Node, Query);
	const FValue* Max = FindValueField(Query, TEXT("BlendSpace.BlendParameters.X.Max"));
	TestTrue(TEXT("X Max is reported"), Max != nullptr && FMath::IsNearlyEqual(Max->NumberValue, 200.0));
	TestNull(TEXT("default Y Max is not reported"), FindValueField(Query, TEXT("BlendSpace.BlendParameters.Y.Max")));
	TestTrue(TEXT("Input Interpolation setting is set"), SetBlendSpaceSetting(*Node, TEXT("BlendSpace.InterpolationParam.X.InterpolationTime"), MakeNumberValue(0.25), Error));
	FAssetDocumentGraphError Unknown;
	TestFalse(TEXT("an unknown setting is rejected"), SetBlendSpaceSetting(*Node, TEXT("BlendSpace.BlendParameters.Z.Max"), MakeNumberValue(1.0), Unknown));
	FAssetDocumentGraphError Hidden;
	TestFalse(TEXT("a setting the panel hides is rejected"), SetBlendSpaceSetting(*Node, TEXT("BlendSpace.Notifies"), MakeArrayValue(), Hidden));

	// Samples: two added with their sample graphs.
	TestTrue(TEXT("two samples are added"), SetSamples(*Node, MakeArrayValue({Sample(TEXT("Idle"), 0.0), Sample(TEXT("Walk"), 100.0)}), Error));
	TestEqual(TEXT("blend space has two samples"), BlendSpace->GetNumberOfBlendSamples(), 2);
	TestEqual(TEXT("one sample graph per sample"), GraphNames(*Node), TArray<FString>({TEXT("Idle"), TEXT("Walk")}));
	TestTrue(TEXT("Walk is at X=100"), BlendSpace->GetNumberOfBlendSamples() == 2 && BlendSpace->GetBlendSample(1).SampleValue.Equals(FVector(100.0, 0.0, 0.0)));
	FValue Listed = MakeObjectValue();
	QueryBlendSpace(*Node, Listed);
	const FValue* Samples = FindValueField(Listed, TEXT("Samples"));
	TestTrue(TEXT("samples are reported in index order"), Samples != nullptr && Samples->Elements.Num() == 2
		&& FindValueField(Samples->Elements[1], TEXT("Name")) != nullptr && FindValueField(Samples->Elements[1], TEXT("Name"))->StringValue == TEXT("Walk"));
	UEdGraph* WalkGraph = GetSampleGraphs(*Node)[1];
	TestTrue(TEXT("Animation Blueprint with samples compiles"), Compiles(*Blueprint));

	// Unchanged samples are a no-op; a move keeps the sample graph; a removal removes it.
	TestTrue(TEXT("unchanged samples apply"), SetSamples(*Node, MakeArrayValue({Sample(TEXT("Idle"), 0.0), Sample(TEXT("Walk"), 100.0)}), Error));
	TestTrue(TEXT("Idle removed, Walk moved, Run added"), SetSamples(*Node, MakeArrayValue({Sample(TEXT("Walk"), 150.0), Sample(TEXT("Run"), 200.0)}), Error));
	TArray<FString> Names = GraphNames(*Node);
	Names.Sort();
	TestEqual(TEXT("sample graphs follow the samples"), Names, TArray<FString>({TEXT("Run"), TEXT("Walk")}));
	TestTrue(TEXT("Walk keeps its sample graph"), GetSampleGraphs(*Node).Contains(WalkGraph));
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	TestFalse(TEXT("the Idle sample graph is gone from the Blueprint"), AllGraphs.ContainsByPredicate([](const UEdGraph* Graph) { return Graph != nullptr && Graph->GetName() == TEXT("Idle"); }));
	const int32 WalkIndex = GetSampleGraphs(*Node).IndexOfByKey(WalkGraph);
	TestTrue(TEXT("Walk sample moved to X=150"), WalkIndex != INDEX_NONE && BlendSpace->GetBlendSample(WalkIndex).SampleValue.Equals(FVector(150.0, 0.0, 0.0)));

	// Rejected sample lists change nothing.
	const int32 Before = BlendSpace->GetNumberOfBlendSamples();
	for (const FValue& Rejected : {
			MakeArrayValue({Sample(TEXT("Walk"), 0.0), Sample(TEXT("walk"), 10.0)}),
			MakeArrayValue({Sample(TEXT("Bad Name"), 0.0)}),
			MakeStringValue(TEXT("Walk"))})
	{
		FAssetDocumentGraphError RejectedError;
		TestFalse(TEXT("an invalid sample list is rejected"), SetSamples(*Node, Rejected, RejectedError));
		TestEqual(TEXT("rejection code"), RejectedError.Code, FString(TEXT("Graph.InvalidArgument")));
	}
	TestEqual(TEXT("rejected edits keep the samples"), BlendSpace->GetNumberOfBlendSamples(), Before);

	// No samples: every sample graph is removed.
	TestTrue(TEXT("samples are cleared"), SetSamples(*Node, MakeArrayValue(), Error));
	TestEqual(TEXT("no samples remain"), BlendSpace->GetNumberOfBlendSamples(), 0);
	TestEqual(TEXT("no sample graphs remain"), GetSampleGraphs(*Node).Num(), 0);
	TestTrue(TEXT("Animation Blueprint without samples compiles"), Compiles(*Blueprint));
	return true;
}

#endif
