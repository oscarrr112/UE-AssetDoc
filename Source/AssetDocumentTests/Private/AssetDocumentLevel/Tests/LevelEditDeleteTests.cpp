// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// edit_level delete (decision D5): referenced actors are refused with
// Level.DeleteReferenced listing what the Delete key's dialog would list (actor
// references, Level Blueprint, soft references such as a Level Sequence binding),
// unless allow_referenced, which deletes and warns. Undo restores.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/PointLight.h"
#include "LevelEditTestSupport.h"
#include "LevelEditTestTypes.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelEditDeleteTests
{
// A saved Level Sequence that possesses Actor (the binding the delete dialog finds
// through IAssetTools::FindSoftReferencesToObjects).
bool CreateBindingSequence(FAutomationTestBase& Test, const FString& PackageName, AActor& Actor)
{
	UPackage* Package = CreatePackage(*PackageName);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *FPackageName::GetShortName(PackageName),
		RF_Public | RF_Standalone | RF_Transactional);
	Sequence->Initialize();
	const FGuid Binding = Sequence->GetMovieScene()->AddPossessable(Actor.GetActorLabel(), Actor.GetClass());
	Sequence->BindPossessableObject(Binding, Actor, Actor.GetWorld());
	FAssetRegistryModule::AssetCreated(Sequence);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	const bool bSaved = UPackage::SavePackage(Package, Sequence, *File, Args);
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({File});
	return Test.TestTrue(TEXT("Level Sequence saved"), bSaved);
}

TArray<FString> ReferencerPaths(const FAssetDocumentDiagnostic* Diagnostic)
{
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* Referencers = nullptr;
	if (Diagnostic != nullptr && Diagnostic->Details.IsValid() && Diagnostic->Details->TryGetArrayField(TEXT("referencers"), Referencers))
	{
		for (const TSharedPtr<FJsonValue>& Referencer : *Referencers)
		{
			Paths.Add(Referencer->AsObject()->GetStringField(TEXT("kind")) + TEXT(" ") + Referencer->AsObject()->GetStringField(TEXT("path")));
		}
	}
	return Paths;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditDeleteTest,
	"UEAssetDoc.Level.Edit.Delete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditDeleteTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	using namespace AssetDocumentLevelEditDeleteTests;
	FScopedTestAsset SequenceAsset(TEXT("LS_Room"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	ULevel& Level = *World->PersistentLevel;
	const FString LampPath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector::ZeroVector, TEXT("Lamp")));
	AAssetDocumentLevelTestActor* Pointer = Cast<AAssetDocumentLevelTestActor>(
		SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector(100, 0, 0), TEXT("Pointer")));
	Pointer->Target = FindActorByPath(LampPath);
	const FString PointerPath = GetActorPath(*Pointer);
	const FString LonePath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(300, 0, 0), TEXT("Lone")));
	const FString BoundPath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(600, 0, 0), TEXT("Bound")));
	TestTrue(TEXT("map saved"), FScopedTestMaps::SaveWorld(*World));
	CreateBindingSequence(*this, SequenceAsset.PackageName, *FindActorByPath(BoundPath));
	ResetUndo();

	// A referenced actor is refused; nothing changes.
	{
		const FLevelToolResult Refused = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "delete", "actors": ["%s", {"label": "Lone"}]}]})"), *LampPath), TEXT("referenced"));
		TestFalse(TEXT("refused"), Refused.bSuccess);
		const FAssetDocumentDiagnostic* Referenced = FindCode(Refused, TEXT("Level.DeleteReferenced"));
		if (TestNotNull(TEXT("Level.DeleteReferenced"), Referenced))
		{
			TestEqual(TEXT("at the referenced actor"), Referenced->Path, FString(TEXT("/ops/0/actors/0")));
			TestEqual(TEXT("lists the referencing actor"), ReferencerPaths(Referenced), TArray<FString>({TEXT("actor ") + PointerPath}));
		}
		TestNotNull(TEXT("lamp still there"), FindActorByPath(LampPath));
		TestNotNull(TEXT("the unreferenced actor of the same op is not deleted either"), FindActorByPath(LonePath));
		TestEqual(TEXT("no Undo entry"), UndoQueueLength(), 0);
	}

	// Deleting the referencer in the same op is no reference.
	{
		const FLevelToolResult Both = Edit(*this, FString::Printf(TEXT(R"({"description": "AI: clear", "ops": [
			{"op": "delete", "actors": ["%s", "%s"]}]})"), *LampPath, *PointerPath), TEXT("delete both"));
		TestTrue(TEXT("deleting referenced and referencer together succeeds"), Both.bSuccess);
		TestNull(TEXT("lamp gone"), FindActorByPath(LampPath));
		TestNull(TEXT("pointer gone"), FindActorByPath(PointerPath));
		TestTrue(TEXT("Remove change"), FindChange(Both, TEXT("/ops/0")).IsValid()
			&& FindChange(Both, TEXT("/ops/0"))->GetStringField(TEXT("kind")) == TEXT("Remove"));
		TestTrue(TEXT("map package dirty"), PayloadStrings(Both, TEXT("dirty_packages")).Contains(Maps.PackageName(TEXT("Room"))));
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		AAssetDocumentLevelTestActor* Restored = Cast<AAssetDocumentLevelTestActor>(FindActorByPath(PointerPath));
		TestTrue(TEXT("Undo restores both with the reference"), Restored != nullptr && FindActorByPath(LampPath) != nullptr
			&& Restored->Target == FindActorByPath(LampPath));
	}

	// allow_referenced deletes and warns.
	{
		const FLevelToolResult Allowed = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "delete", "actors": ["%s"], "allow_referenced": true}]})"), *LampPath), TEXT("allow_referenced"));
		TestTrue(TEXT("allowed"), Allowed.bSuccess);
		const FAssetDocumentDiagnostic* Warning = FindCode(Allowed, TEXT("Level.DeletedActorWasReferenced"));
		TestTrue(TEXT("warning"), Warning != nullptr && Warning->Severity == TEXT("warning"));
		TestNull(TEXT("lamp deleted"), FindActorByPath(LampPath));
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	}

	// A Level Sequence binding is a soft referencer (design R5).
	{
		const FLevelToolResult Bound = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "delete", "actors": ["%s"]}]})"), *BoundPath), TEXT("sequence-bound actor"));
		const TArray<FString> Referencers = ReferencerPaths(FindCode(Bound, TEXT("Level.DeleteReferenced")));
		AddInfo(FString::Printf(TEXT("referencers of the bound actor: %s"), *FString::Join(Referencers, TEXT(", "))));
		TestTrue(TEXT("the Level Sequence binding is reported"), Referencers.ContainsByPredicate([&SequenceAsset](const FString& Entry)
		{
			return Entry.StartsWith(TEXT("soft ")) && Entry.Contains(SequenceAsset.PackageName);
		}));
		TestNotNull(TEXT("bound actor kept"), FindActorByPath(BoundPath));
	}

	// Batch ordering: a spawned actor can be deleted later in the batch; an actor deleted
	// earlier in the batch cannot be used.
	{
		const FLevelToolResult Ordered = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "spawn", "id": "temp", "class": "/Script/Engine.PointLight"},
			{"op": "delete", "actors": ["@temp", "%s"]}]})"), *LonePath), TEXT("spawn then delete"));
		TestTrue(TEXT("spawn then delete succeeds"), Ordered.bSuccess);
		TestNull(TEXT("lone deleted"), FindActorByPath(LonePath));
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		TestNotNull(TEXT("lone restored"), FindActorByPath(LonePath));

		const FLevelToolResult Twice = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "delete", "actors": ["%s"]},
			{"op": "set", "actor": "%s", "label": "Ghost"}]})"), *LonePath, *LonePath), TEXT("use after delete"));
		TestTrue(TEXT("use after delete refused"), HasCode(Twice, TEXT("Level.ActorDeletedEarlierInBatch"), TEXT("/ops/1/actor")));
		TestNotNull(TEXT("lone still there"), FindActorByPath(LonePath));
	}

	// References the batch makes or clears before the delete: a set that
	// points the referencer elsewhere lifts the refusal; an actor spawned (or set) earlier
	// in the batch to reference the deleted actor is a referencer.
	{
		const FLevelToolResult Moved = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "properties": {"Target": "%s"}},
			{"op": "delete", "actors": ["%s"]}]})"), *PointerPath, *LonePath, *LampPath), TEXT("reference moved first"));
		TestTrue(TEXT("a reference moved away earlier in the batch is no reference"), Moved.bSuccess);
		TestNull(TEXT("lamp deleted"), FindActorByPath(LampPath));
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());

		const FLevelToolResult Spawned = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "spawn", "id": "ref", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor", "properties": {"Target": "%s"}},
			{"op": "set", "actor": "%s", "properties": {"Target": "%s"}},
			{"op": "delete", "actors": ["%s"]}]})"), *LonePath, *PointerPath, *LonePath, *LonePath), TEXT("referencers made in the batch"));
		TestFalse(TEXT("refused"), Spawned.bSuccess);
		const FAssetDocumentDiagnostic* Referenced = FindCode(Spawned, TEXT("Level.DeleteReferenced"));
		TArray<FString> Referencers = ReferencerPaths(Referenced);
		Referencers.Sort();
		TestEqual(TEXT("lists the spawned and the set referencer"), Referencers,
			TArray<FString>({TEXT("actor ") + PointerPath, TEXT("actor @ref")}));
		TestEqual(TEXT("phase preflight"), PayloadString(Spawned, TEXT("phase")), FString(TEXT("preflight")));
		TestNotNull(TEXT("lone kept"), FindActorByPath(LonePath));
	}
	return true;
}

#endif
