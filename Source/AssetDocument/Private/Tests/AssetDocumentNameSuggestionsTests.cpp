// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.RuntimeContracts.NameSuggestions: the "Close: ..." candidates of not-found diagnostics.

#include "AssetDocumentNameSuggestions.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNameSuggestionsTest, "UEAssetDoc.RuntimeContracts.NameSuggestions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNameSuggestionsTest::RunTest(const FString& Parameters)
{
	using namespace UE::AssetDocument;
	const TArray<FString> Labels = {TEXT("Floor"), TEXT("Wall_N"), TEXT("Wall_S"), TEXT("CeilingLight"), TEXT("Ceiling"), TEXT("BP_Door"),
		TEXT("CameraActor")};
	// The trial's mistake: a label that shares a word with the real one.
	const TArray<FString> Lamp = SuggestCloseNames(TEXT("CeilingLamp"), Labels);
	TestTrue(TEXT("CeilingLamp suggests CeilingLight"), Lamp.Contains(TEXT("CeilingLight")));
	TestTrue(TEXT("and Ceiling"), Lamp.Contains(TEXT("Ceiling")));
	TestFalse(TEXT("but not Floor"), Lamp.Contains(TEXT("Floor")));
	TestEqual(TEXT("case-insensitive equal first"), SuggestCloseNames(TEXT("floor"), Labels)[0], FString(TEXT("Floor")));
	TestEqual(TEXT("a typo"), SuggestCloseNames(TEXT("CameraActr"), Labels)[0], FString(TEXT("CameraActor")));
	TestEqual(TEXT("swapped letters are one typo"), SuggestCloseNames(TEXT("Spien"), {TEXT("Spine"), TEXT("Neck")}), TArray<FString>{TEXT("Spine")});
	TestEqual(TEXT("containment"), SuggestCloseNames(TEXT("Door"), Labels)[0], FString(TEXT("BP_Door")));
	TestTrue(TEXT("nothing close"), SuggestCloseNames(TEXT("Zebra"), Labels).IsEmpty());

	// Bones: the physics trial's "head_bone" for "head".
	const TArray<FString> Bones = {TEXT("root"), TEXT("pelvis"), TEXT("spine_01"), TEXT("neck_01"), TEXT("head"), TEXT("hand_l")};
	TestEqual(TEXT("head_bone -> head"), SuggestCloseNames(TEXT("head_bone"), Bones)[0], FString(TEXT("head")));

	// A shared naming prefix does not make a name close (every MetaSound Source shares "MSS"; a Preset.Parent
	// "MSS_Plucky" must not list unrelated sources after MSS_Pluck).
	const TArray<FString> Sources = {TEXT("MSS_PlayRandom_Oneshot"), TEXT("MSS_PlayRandom_Loop"), TEXT("MSS_Pluck"), TEXT("MSS_Pluck_Dark"),
		TEXT("MSS_MIDISequencePlayer"), TEXT("MSS_MetronomeClockGenerator"), TEXT("MSS_Pad"), TEXT("MSS_Drone")};
	TestEqual(TEXT("MSS_Plucky: the source, then the preset sharing 'Pluck', nothing else"), SuggestCloseNames(TEXT("MSS_Plucky"), Sources),
		TArray<FString>{TEXT("MSS_Pluck"), TEXT("MSS_Pluck_Dark")});
	TestEqual(TEXT("a distinctive word still finds its names"), SuggestCloseNames(TEXT("MSS_PlayRandom"), Sources),
		TArray<FString>{TEXT("MSS_PlayRandom_Loop"), TEXT("MSS_PlayRandom_Oneshot")});
	TestTrue(TEXT("only the prefix in common: nothing close"), SuggestCloseNames(TEXT("MSS_Zebra"), Sources).IsEmpty());
	// Element keys share their type word.
	const TArray<FString> Elements = {TEXT("Bone(root)"), TEXT("Bone(pelvis)"), TEXT("Bone(spine_01)"), TEXT("Bone(spine_02)"), TEXT("Bone(hand_l)"),
		TEXT("Control(hand_l_ctrl)")};
	TestEqual(TEXT("Bone(spine_03): the spine bones"), SuggestCloseNames(TEXT("Bone(spine_03)"), Elements),
		TArray<FString>{TEXT("Bone(spine_01)"), TEXT("Bone(spine_02)")});
	TestEqual(TEXT("Bone(hand_r): hand_l first"), SuggestCloseNames(TEXT("Bone(hand_r)"), Elements)[0], FString(TEXT("Bone(hand_l)")));

	// The sentence: close names, else the list, else nothing.
	TestEqual(TEXT("close sentence"), DescribeNameCandidates(TEXT("LightComponent"), {TEXT("LightComponent0"), TEXT("Root")}, TEXT("Components")),
		FString(TEXT(" Close: LightComponent0.")));
	TestEqual(TEXT("list sentence"), DescribeNameCandidates(TEXT("Xyz"), {TEXT("B"), TEXT("A")}, TEXT("Components")),
		FString(TEXT(" Components: A, B.")));
	TestEqual(TEXT("long list is cut"), DescribeNameCandidates(TEXT("Xyz"), {TEXT("C"), TEXT("B"), TEXT("A")}, TEXT("Bones"), 2),
		FString(TEXT(" Bones: A, B (+1 more).")));
	TestEqual(TEXT("no candidates"), DescribeNameCandidates(TEXT("Xyz"), TArray<FString>(), TEXT("Bones")), FString());
	return true;
}

#endif
