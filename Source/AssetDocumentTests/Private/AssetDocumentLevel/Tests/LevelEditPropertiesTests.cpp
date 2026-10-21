// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// edit_level set: properties of actors, native components, Blueprint (SCS) components
// and instanced subobjects of EditInlineNew components, with float normalisation
// (design R7) and the verify step that rolls back a value the editor changed after
// the write (Level.PropertyValueNotStoredExactly): a PostEditChange clamp and a
// construction script.

#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "LevelEditTestSupport.h"
#include "LevelEditTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditPropertiesTest,
	"UEAssetDoc.Level.Edit.Properties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditPropertiesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestAsset Door(TEXT("BP_Door"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World)
		|| !Door.Apply(*this, DoorBlueprintDocument(Door.PackageName, TEXT("Create"), 1.0), TEXT("create BP_Door")))
	{
		return false;
	}
	ULevel& Level = *World->PersistentLevel;
	const FString LampPath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(0, 0, 200), TEXT("Lamp")));
	const FString ProbePath = GetActorPath(*SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector::ZeroVector, TEXT("Probe")));
	UClass* DoorClass = LoadObject<UClass>(nullptr, *Door.ClassPath());
	if (!TestNotNull(TEXT("door class"), DoorClass))
	{
		return false;
	}
	const FString DoorPath = GetActorPath(*SpawnActor(Level, DoorClass, FVector(400, 0, 0), TEXT("Door")));
	ResetUndo();

	// Floats are normalised as documents are: 0.123456789 is stored as the float
	// 0.12345679 and verifies as that.
	{
		const FLevelToolResult Result = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "components": {"LightComponent0": {"properties": {"Intensity": 0.123456789,
			  "LightColor": {"G": 10}}}}}]})"), *LampPath), TEXT("float normalisation"));
		TestTrue(TEXT("float write succeeds"), Result.bSuccess);
		APointLight* Lamp = Cast<APointLight>(FindActorByPath(LampPath));
		TestEqual(TEXT("stored float"), Lamp->PointLightComponent->Intensity, 0.123456789f);
		const TSharedPtr<FJsonObject> Change = FindChange(Result, TEXT("/ops/0/components/LightComponent0/properties/Intensity"));
		TestTrue(TEXT("the change shows the stored value"), Change.IsValid()
			&& JsonText(Change->TryGetField(TEXT("after"))) == TEXT("0.12345679"));
		TestEqual(TEXT("partial struct: other members kept"), Lamp->PointLightComponent->LightColor, FColor(255, 10, 255, 255));
	}

	// EditInlineNew component placed in a level: EditInstanceOnly values and
	// an instanced settings object, written with instance semantics.
	{
		const FLevelToolResult Result = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "components": {"TestComponent": {"properties": {
			  "InstanceOnlyValue": 2.5,
			  "Settings": {"Class": "/Script/AssetDocumentTests.AssetDocumentLevelTestSettings", "Properties": {"Strength": 3}}}}}}]})"),
			*ProbePath), TEXT("instanced"));
		TestTrue(TEXT("instance-only and instanced writes succeed"), Result.bSuccess);
		AAssetDocumentLevelTestActor* Probe = Cast<AAssetDocumentLevelTestActor>(FindActorByPath(ProbePath));
		TestEqual(TEXT("instance-only value"), Probe->TestComponent->InstanceOnlyValue, 2.5f);
		UAssetDocumentLevelTestSettings* Settings = Probe->TestComponent->Settings;
		if (TestNotNull(TEXT("instanced settings object"), Settings))
		{
			TestEqual(TEXT("its outer is the component"), Settings->GetOuter(), static_cast<UObject*>(Probe->TestComponent.Get()));
			TestEqual(TEXT("strength"), Settings->Strength, 3.0f);
		}
		// Edit the existing instanced object (the instanced value form always names its class).
		const FLevelToolResult Again = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "components": {"TestComponent": {"properties": {
			  "Settings": {"Class": "/Script/AssetDocumentTests.AssetDocumentLevelTestSettings", "Properties": {"Strength": 4.5}}}}}}]})"),
			*ProbePath), TEXT("instanced edit"));
		TestTrue(TEXT("instanced edit succeeds"), Again.bSuccess);
		Probe = Cast<AAssetDocumentLevelTestActor>(FindActorByPath(ProbePath));
		TestTrue(TEXT("strength edited"), Probe->TestComponent->Settings != nullptr && Probe->TestComponent->Settings->Strength == 4.5f);
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		Probe = Cast<AAssetDocumentLevelTestActor>(FindActorByPath(ProbePath));
		TestTrue(TEXT("Undo restores the strength"), Probe->TestComponent->Settings != nullptr && Probe->TestComponent->Settings->Strength == 3.0f);
		TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
	}

	// Blueprint instance: an SCS component override survives the construction-script
	// reruns of later writes in the same batch and of later batches.
	{
		const FLevelToolResult Result = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "components": {"Lamp": {"properties": {"Intensity": 4321}}}},
			{"op": "set", "actor": "%s", "properties": {"Brightness": 7.25, "LinkedLight": "%s"}}]})"),
			*DoorPath, *DoorPath, *LampPath), TEXT("Blueprint instance"));
		TestTrue(TEXT("Blueprint writes succeed"), Result.bSuccess);
		AActor* DoorActor = FindActorByPath(DoorPath);
		TestEqual(TEXT("variable"), ReadValue(DoorActor, TEXT("Brightness")), FString(TEXT("7.25")));
		TestEqual(TEXT("actor reference variable"), ReadValue(DoorActor, TEXT("LinkedLight")), LampPath);
		TestEqual(TEXT("component override after the rerun"), ReadValue(FindObjectFast<UObject>(DoorActor, TEXT("Lamp")), TEXT("Intensity")),
			FString(TEXT("4321")));
		const FLevelToolResult Later = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "transform": {"location": {"Y": 50}}, "properties": {"bStartsOpen": false}}]})"),
			*DoorPath), TEXT("later batch"));
		TestTrue(TEXT("later batch succeeds"), Later.bSuccess);
		DoorActor = FindActorByPath(DoorPath);
		TestEqual(TEXT("override kept across batches"), ReadValue(FindObjectFast<UObject>(DoorActor, TEXT("Lamp")), TEXT("Intensity")),
			FString(TEXT("4321")));
		TestEqual(TEXT("partial location"), DoorActor->GetActorLocation(), FVector(400, 50, 0));
	}

	// Refusals in preflight: nothing changes.
	{
		const int32 QueueBefore = UndoQueueLength();
		const FLevelToolResult Refused = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "properties": {"bDefaultsOnly": true}},
			{"op": "set", "actor": "%s", "properties": {"Secret": 3}},
			{"op": "set", "actor": "%s", "components": {"LightComponent0": {"properties": {"Intensty": 1}}}},
			{"op": "set", "actor": "%s", "components": {"LightComponent0": {"properties": {"LightColor": {"R": "red"}}}}},
			{"op": "set", "actor": "%s", "components": {"NoSuchComponent": {"properties": {"Intensity": 1}}}}]})"),
			*ProbePath, *DoorPath, *LampPath, *LampPath, *LampPath), TEXT("refusals"));
		TestFalse(TEXT("refused"), Refused.bSuccess);
		TestTrue(TEXT("EditDefaultsOnly"), HasCode(Refused, TEXT("Level.PropertyNotEditableOnInstance"), TEXT("/ops/0/properties/bDefaultsOnly")));
		TestTrue(TEXT("Blueprint variable without Instance Editable"),
			HasCode(Refused, TEXT("Level.PropertyNotEditableOnInstance"), TEXT("/ops/1/properties/Secret")));
		TestTrue(TEXT("unknown property"),
			HasCode(Refused, TEXT("Generic.UnknownProperty"), TEXT("/ops/2/components/LightComponent0/properties/Intensty")));
		const bool bTypedError = Refused.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Path.StartsWith(TEXT("/ops/3/components/LightComponent0/properties/LightColor"));
		});
		TestTrue(TEXT("a wrongly typed struct member is reported inside the value"), bTypedError);
		TestTrue(TEXT("unknown component"), HasCode(Refused, TEXT("Level.ComponentNotFound"), TEXT("/ops/4/components/NoSuchComponent")));
		TestEqual(TEXT("phase preflight"), PayloadString(Refused, TEXT("phase")), FString(TEXT("preflight")));
		TestEqual(TEXT("no Undo entry"), UndoQueueLength(), QueueBefore);
	}

	// Values the editor changes after the write: the batch rolls back.
	{
		const int32 QueueBefore = UndoQueueLength();
		APointLight* Lamp = Cast<APointLight>(FindActorByPath(LampPath));
		const float LampBefore = Lamp->PointLightComponent->Intensity;
		const FLevelToolResult Clamped = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "components": {"LightComponent0": {"properties": {"Intensity": 777}}}},
			{"op": "set", "actor": "%s", "components": {"TestComponent": {"properties": {"Clamped": 50}}}}]})"),
			*LampPath, *ProbePath), TEXT("PostEditChange clamp"));
		TestFalse(TEXT("clamped value fails"), Clamped.bSuccess);
		const FAssetDocumentDiagnostic* NotStored = FindCode(Clamped, TEXT("Level.PropertyValueNotStoredExactly"));
		if (TestNotNull(TEXT("PropertyValueNotStoredExactly"), NotStored))
		{
			TestEqual(TEXT("at the value"), NotStored->Path, FString(TEXT("/ops/1/components/TestComponent/properties/Clamped")));
			TestTrue(TEXT("names the stored value"), NotStored->Details.IsValid()
				&& JsonText(NotStored->Details->TryGetField(TEXT("stored"))) == TEXT("10"));
		}
		TestEqual(TEXT("phase verify"), PayloadString(Clamped, TEXT("phase")), FString(TEXT("verify")));
		TestTrue(TEXT("rolled back"), PayloadBool(Clamped, TEXT("rolled_back")));
		TestEqual(TEXT("rollback check"), PayloadString(Clamped, TEXT("rollback_check")), FString(TEXT("equal")));
		Lamp = Cast<APointLight>(FindActorByPath(LampPath));
		TestEqual(TEXT("the earlier op of the batch is undone too"), Lamp->PointLightComponent->Intensity, LampBefore);
		TestEqual(TEXT("clamped value undone"), Cast<AAssetDocumentLevelTestActor>(FindActorByPath(ProbePath))->TestComponent->Clamped, 0.0f);
		TestEqual(TEXT("no Undo entry left"), UndoQueueLength(), QueueBefore);

		const FLevelToolResult Construction = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "properties": {"Score": 500}}]})"), *ProbePath), TEXT("construction script"));
		TestTrue(TEXT("construction script rewrite fails"), HasCode(Construction, TEXT("Level.PropertyValueNotStoredExactly"),
			TEXT("/ops/0/properties/Score")));
		TestEqual(TEXT("score undone"), Cast<AAssetDocumentLevelTestActor>(FindActorByPath(ProbePath))->Score, 0.0f);
		TestTrue(TEXT("a value within the limit is fine"), Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "properties": {"Score": 99.5}}]})"), *ProbePath), TEXT("score ok")).bSuccess);
	}

	// dry_run: planned changes, nothing touched.
	{
		const int32 QueueBefore = UndoQueueLength();
		const TArray<FString> ActorsBefore = ListedActorPaths(Level);
		const FLevelToolResult Dry = Edit(*this, FString::Printf(TEXT(R"({"dry_run": true, "ops": [
			{"op": "spawn", "id": "extra", "class": "/Script/Engine.PointLight", "components": {"LightComponent0": {"properties": {"Intensity": 9}}}},
			{"op": "set", "actor": "%s", "label": "Renamed", "components": {"LightComponent0": {"properties": {"Intensity": 42}}}}]})"),
			*LampPath), TEXT("dry run"));
		TestTrue(TEXT("dry run succeeds"), Dry.bSuccess);
		TestTrue(TEXT("dry_run flag"), PayloadBool(Dry, TEXT("dry_run")));
		TestEqual(TEXT("would_spawn"), static_cast<int32>(Dry.Payload->GetNumberField(TEXT("would_spawn"))), 1);
		const TSharedPtr<FJsonObject> Planned = FindChange(Dry, TEXT("/ops/1/components/LightComponent0/properties/Intensity"));
		TestTrue(TEXT("planned property change"), Planned.IsValid() && JsonText(Planned->TryGetField(TEXT("after"))) == TEXT("42"));
		const TSharedPtr<FJsonObject> PlannedSpawn = FindChange(Dry, TEXT("/ops/0/components/LightComponent0/properties/Intensity"));
		TestTrue(TEXT("planned change on the actor to spawn: before = archetype"), PlannedSpawn.IsValid()
			&& JsonText(PlannedSpawn->TryGetField(TEXT("before"))) == TEXT("5000"));
		TestEqual(TEXT("dry run touches nothing"), ListedActorPaths(Level), ActorsBefore);
		TestEqual(TEXT("label unchanged"), FindActorByPath(LampPath)->GetActorLabel(false), FString(TEXT("Lamp")));
		TestEqual(TEXT("no Undo entry"), UndoQueueLength(), QueueBefore);
	}
	return true;
}

#endif
