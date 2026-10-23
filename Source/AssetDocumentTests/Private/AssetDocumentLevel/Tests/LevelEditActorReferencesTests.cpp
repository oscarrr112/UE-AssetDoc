// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// edit_level actor references (design §4): "@id" of an actor created earlier in the
// batch into an AActor* property, same-level paths, cross-level hard references
// refused, soft references across levels accepted, unknown ids refused.

#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "LevelEditTestSupport.h"
#include "LevelEditTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditActorReferencesTest,
	"UEAssetDoc.Level.Edit.ActorReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditActorReferencesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	ULevel* Sub = World != nullptr ? Maps.AddSublevel(*World, TEXT("Sub")) : nullptr;
	if (!TestNotNull(TEXT("map with a sublevel"), Sub))
	{
		return false;
	}
	const FString HerePath = GetActorPath(*SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector::ZeroVector, TEXT("Here")));
	const FString ThereSubPath = GetActorPath(*SpawnActor(*Sub, APointLight::StaticClass(), FVector(500, 0, 0), TEXT("There")));
	ResetUndo();

	// "@id" into a hard reference, while both actors exist only in the plan (preflight
	// decodes with class default objects standing in for them).
	const FLevelToolResult Result = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "spawn", "id": "a", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor", "label": "A"},
		{"op": "spawn", "id": "b", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor", "label": "B",
		 "properties": {"Target": "@a", "SoftTarget": "%s"}},
		{"op": "set", "actor": "@a", "properties": {"Target": "%s", "SoftTarget": "@b"}}]})"), *ThereSubPath, *HerePath),
		TEXT("references"));
	if (!TestTrue(TEXT("references succeed"), Result.bSuccess))
	{
		return false;
	}
	AAssetDocumentLevelTestActor* A = Cast<AAssetDocumentLevelTestActor>(BatchActor(Result, TEXT("a")));
	AAssetDocumentLevelTestActor* B = Cast<AAssetDocumentLevelTestActor>(BatchActor(Result, TEXT("b")));
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B))
	{
		return false;
	}
	TestEqual(TEXT("@a in a hard reference"), B->Target.Get(), static_cast<AActor*>(A));
	TestEqual(TEXT("a soft reference into another level"), B->SoftTarget.ToSoftObjectPath().ToString(), ThereSubPath);
	TestEqual(TEXT("a same-level path"), GetActorPath(*A->Target), HerePath);
	TestEqual(TEXT("@b in a soft reference"), A->SoftTarget.Get(), static_cast<AActor*>(B));
	const TSharedPtr<FJsonObject> Change = FindChange(Result, TEXT("/ops/1/properties/Target"));
	TestTrue(TEXT("the change names the actor's path"), Change.IsValid() && Change->GetStringField(TEXT("after")) == GetActorPath(*A));

	const FString APath = GetActorPath(*A);
	const int32 QueueBefore = UndoQueueLength();
	const FLevelToolResult Refused = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "set", "actor": "%s", "properties": {"Target": "%s"}},
		{"op": "set", "actor": "%s", "properties": {"Target": "@nope"}}]})"),
		*APath, *ThereSubPath, *APath), TEXT("refusals"));
	TestFalse(TEXT("refused"), Refused.bSuccess);
	TestTrue(TEXT("a hard reference into another level"), HasCode(Refused, TEXT("Level.CrossLevelHardReference"), TEXT("/ops/0/properties/Target")));
	TestTrue(TEXT("an unknown batch id"), HasCode(Refused, TEXT("Level.UnknownId"), TEXT("/ops/1/properties/Target")));
	TestEqual(TEXT("phase preflight"), PayloadString(Refused, TEXT("phase")), FString(TEXT("preflight")));
	TestEqual(TEXT("no Undo entry"), UndoQueueLength(), QueueBefore);
	TestEqual(TEXT("A's target is unchanged"), GetActorPath(*Cast<AAssetDocumentLevelTestActor>(FindActorByPath(APath))->Target), HerePath);

	// An "@id" of an actor spawned into another level, into a hard reference: preflight
	// knows the level the actor will be in, so nothing is applied.
	const FLevelToolResult CrossBatch = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "spawn", "id": "far", "class": "/Script/Engine.PointLight", "level": "%s"},
		{"op": "set", "actor": "%s", "properties": {"Target": "@far"}}]})"), *Maps.PackageName(TEXT("Sub")), *APath),
		TEXT("cross-level batch id"));
	TestTrue(TEXT("cross-level @id refused"), HasCode(CrossBatch, TEXT("Level.CrossLevelHardReference"), TEXT("/ops/1/properties/Target")));
	TestEqual(TEXT("cross-level @id: phase preflight"), PayloadString(CrossBatch, TEXT("phase")), FString(TEXT("preflight")));
	TestFalse(TEXT("nothing to roll back"), PayloadBool(CrossBatch, TEXT("rolled_back")));
	TestEqual(TEXT("no Undo entry"), UndoQueueLength(), QueueBefore);
	TestEqual(TEXT("no leftover actor in the sublevel"), ListedActorPaths(*Sub), TArray<FString>({ThereSubPath}));

	// The same "@id" spawned into this level is accepted (the stand-in is not judged by
	// its own outer), and a class default object is no placed actor.
	const FLevelToolResult SameBatch = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "spawn", "id": "near", "class": "/Script/Engine.PointLight"},
		{"op": "set", "actor": "%s", "properties": {"Target": "@near"}}]})"), *APath), TEXT("same-level batch id"));
	TestTrue(TEXT("same-level @id accepted"), SameBatch.bSuccess);
	TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	const FLevelToolResult Template = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "set", "actor": "%s", "properties": {"Target": "/Script/Engine.Default__PointLight"}}]})"), *APath),
		TEXT("class default object"));
	TestTrue(TEXT("a class default object is Level.NotALevelActor"), HasCode(Template, TEXT("Level.NotALevelActor"),
		TEXT("/ops/0/properties/Target")));
	TestEqual(TEXT("class default object: phase preflight"), PayloadString(Template, TEXT("phase")), FString(TEXT("preflight")));

	// A label names the actor that has it before the request: earlier ops that move the
	// label do not redirect a later op, in the dry run, the apply or "after".
	const FString XPath = GetActorPath(*SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(0, 300, 0), TEXT("Lamp")));
	const FString YPath = GetActorPath(*SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(0, 600, 0), TEXT("Spare")));
	const FString Relabel = FString::Printf(TEXT(R"({"ops": [
		{"op": "set", "actor": "%s", "label": "Old"},
		{"op": "set", "actor": "%s", "label": "Lamp"},
		{"op": "set", "actor": {"label": "Lamp"}, "components": {"LightComponent0": {"properties": {"Intensity": 77}}}}],
		"after": {"select": [{"label": "Lamp"}]}})"), *XPath, *YPath);
	const FLevelToolResult DryRelabel = Edit(*this, Relabel.Replace(TEXT("{\"ops\""), TEXT("{\"dry_run\": true, \"ops\"")), TEXT("relabel dry run"));
	const TSharedPtr<FJsonObject> Planned = FindChange(DryRelabel, TEXT("/ops/2/components/LightComponent0/properties/Intensity"));
	TestTrue(TEXT("dry run: the label's actor before the request"), Planned.IsValid() && Planned->GetStringField(TEXT("actor")) == XPath);
	const FLevelToolResult Relabeled = Edit(*this, Relabel, TEXT("relabel"));
	TestTrue(TEXT("relabel succeeds"), Relabeled.bSuccess);
	const APointLight* X = Cast<APointLight>(FindActorByPath(XPath));
	const APointLight* Y = Cast<APointLight>(FindActorByPath(YPath));
	TestTrue(TEXT("apply edits the same actor as the dry run"), X != nullptr && Y != nullptr
		&& X->PointLightComponent->Intensity == 77.0f && Y->PointLightComponent->Intensity != 77.0f);
	TestEqual(TEXT("after.select uses the same actor"), PayloadStrings(Relabeled, TEXT("selection")), TArray<FString>({XPath}));
	return true;
}

// edit_level takes a bare Outliner label or object name wherever it takes an actor path, as query_level / query_play do
// "set" with properties, "delete", after.select. A label shared by two actors is ambiguous
// (Level.ActorRefAmbiguous, details.candidates); an unknown name names the close ones (Level.ActorNotFound).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditBareActorNamesTest,
	"UEAssetDoc.Level.Edit.BareActorNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditBareActorNamesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	ULevel& Level = *World->PersistentLevel;
	const FString LampPath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(0, 0, 200), TEXT("KeyLight")));
	AActor* Probe = SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector::ZeroVector, TEXT("Probe"));
	const FString ProbePath = GetActorPath(*Probe);
	const FString ProbeName = Probe->GetName();
	const FString DupA = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(300, 0, 0), TEXT("Dup")));
	const FString DupB = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(600, 0, 0), TEXT("Dup")));
	ResetUndo();

	// A bare label (set with properties and components), a bare object name, and after.select by label.
	const FLevelToolResult Set = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "set", "actor": "KeyLight", "properties": {"bHidden": true}, "components": {"LightComponent0": {"properties": {"Intensity": 1234}}}},
		{"op": "set", "actor": "%s", "properties": {"Score": 42}}],
		"after": {"select": ["KeyLight"]}})"), *ProbeName), TEXT("bare names"));
	TestTrue(TEXT("a bare label and a bare object name resolve"), Set.bSuccess);
	APointLight* Lamp = Cast<APointLight>(FindActorByPath(LampPath));
	TestTrue(TEXT("the labelled actor's properties are written"), Lamp != nullptr && Lamp->IsHidden()
		&& Lamp->PointLightComponent->Intensity == 1234.0f);
	TestEqual(TEXT("the named actor's property is written"), ReadValue(FindActorByPath(ProbePath), TEXT("Score")), FString(TEXT("42")));
	TestEqual(TEXT("after.select by label"), PayloadStrings(Set, TEXT("selection")), TArray<FString>({LampPath}));
	const TSharedPtr<FJsonObject> Change = FindChange(Set, TEXT("/ops/0/properties/bHidden"));
	TestTrue(TEXT("the change names the actor's path"), Change.IsValid() && Change->GetStringField(TEXT("actor")) == LampPath);

	// The same ambiguity and not-found errors as query_level.
	const int32 QueueBefore = UndoQueueLength();
	const FLevelToolResult Ambiguous = Edit(*this, TEXT(R"({"ops": [{"op": "set", "actor": "Dup", "properties": {"bHidden": true}}]})"),
		TEXT("ambiguous label"));
	TestTrue(TEXT("a label shared by two actors is ambiguous"), HasCode(Ambiguous, TEXT("Level.ActorRefAmbiguous"), TEXT("/ops/0/actor")));
	if (const FAssetDocumentDiagnostic* Diagnostic = FindCode(Ambiguous, TEXT("Level.ActorRefAmbiguous")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		TestTrue(TEXT("both candidates are named"), Diagnostic->Details.IsValid()
			&& Diagnostic->Details->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates->Num() == 2);
	}
	const FLevelToolResult Unknown = Edit(*this, TEXT(R"({"ops": [{"op": "set", "actor": "KeyLite", "properties": {"bHidden": false}}]})"),
		TEXT("unknown name"));
	TestTrue(TEXT("an unknown bare name"), HasCode(Unknown, TEXT("Level.ActorNotFound"), TEXT("/ops/0/actor")));
	if (const FAssetDocumentDiagnostic* Diagnostic = FindCode(Unknown, TEXT("Level.ActorNotFound")))
	{
		TestTrue(TEXT("the message names label and object name"), Diagnostic->Message.Contains(TEXT("labelled or named")));
		TestTrue(TEXT("and the close label"), Diagnostic->Message.Contains(TEXT("KeyLight")));
	}
	TestEqual(TEXT("refused batches leave no Undo entry"), UndoQueueLength(), QueueBefore);

	// delete by a bare label; the other Dup is then unique.
	const FLevelToolResult Deleted = Edit(*this, TEXT(R"({"ops": [{"op": "delete", "actors": ["KeyLight"]}]})"), TEXT("delete by label"));
	TestTrue(TEXT("delete by a bare label"), Deleted.bSuccess);
	TestNull(TEXT("the labelled actor is gone"), FindActorByPath(LampPath));
	TestTrue(TEXT("the Dups are untouched"), FindActorByPath(DupA) != nullptr && FindActorByPath(DupB) != nullptr);
	return true;
}

#endif
