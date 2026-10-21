// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// edit_level set of a table row handle and an input key the editor's pickers would not offer (for example
// RowName "Bost" for "Boost"; the pickup then did nothing in PIE): refused by the dry run and by the edit, naming the
// close names, with nothing changed; valid values are written. The actor class is a test type of AssetDocumentEditor
// (AssetDocumentEditorPickValueTestTypes.h), loaded by path.

#include "LevelEditTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditPickValuesTest,
	"UEAssetDoc.Level.Edit.PickValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditPickValuesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestAsset Table(TEXT("DT_Pickups"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	UClass* ActorClass = LoadObject<UClass>(nullptr, TEXT("/Script/AssetDocumentTests.AssetDocumentPickTestActor"));
	if (!TestNotNull(TEXT("map created"), World) || !TestNotNull(TEXT("test actor class"), ActorClass)
		|| !Table.Apply(*this, FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Engine.DataTable",
			"Action": "Create", "Properties": {}, "Body": {"RowStruct": "/Script/AssetDocumentTests.AssetDocumentPickTestRow",
			"Rows": [{"Name": "Boost"}, {"Name": "Heal"}]}})"), *Table.PackageName), TEXT("create DT_Pickups")))
	{
		return false;
	}
	const FString PickupPath = GetActorPath(*SpawnActor(*World->PersistentLevel, ActorClass, FVector::ZeroVector, TEXT("Pickup")));
	ResetUndo();
	const auto Set = [&](bool bDryRun, const FString& Properties)
		{
			return FString::Printf(TEXT(R"({%s"ops": [{"op": "set", "actor": "%s", "properties": %s}]})"),
				bDryRun ? TEXT("\"dry_run\": true, ") : TEXT(""), *PickupPath, *Properties);
		};
	const FString BadRow = FString::Printf(TEXT(R"({"Pickup": {"DataTable": "%s", "RowName": "Bost"}})"), *Table.ObjectPath());
	const FString BadKey = TEXT(R"({"Key": "Spacbar"})");
	for (const bool bDryRun : {true, false})
	{
		const TCHAR* Mode = bDryRun ? TEXT("dry run") : TEXT("edit");
		const int32 QueueBefore = UndoQueueLength();
		const FLevelToolResult Row = Edit(*this, Set(bDryRun, BadRow), Mode);
		TestFalse(*FString::Printf(TEXT("%s: a missing row is refused"), Mode), Row.bSuccess);
		const FAssetDocumentDiagnostic* RowProblem = FindCode(Row, TEXT("Property.RowNotFound"));
		TestTrue(*FString::Printf(TEXT("%s: the row diagnostic names the close row"), Mode),
			RowProblem != nullptr && RowProblem->Message.Contains(TEXT("Close: Boost")) && RowProblem->Path.Contains(TEXT("Pickup")));
		const FLevelToolResult Key = Edit(*this, Set(bDryRun, BadKey), Mode);
		TestFalse(*FString::Printf(TEXT("%s: a missing key is refused"), Mode), Key.bSuccess);
		const FAssetDocumentDiagnostic* KeyProblem = FindCode(Key, TEXT("Property.KeyNotFound"));
		TestTrue(*FString::Printf(TEXT("%s: the key diagnostic names SpaceBar"), Mode),
			KeyProblem != nullptr && KeyProblem->Message.Contains(TEXT("SpaceBar")));
		TestEqual(*FString::Printf(TEXT("%s: nothing recorded"), Mode), UndoQueueLength(), QueueBefore);
	}
	AActor* Pickup = FindActorByPath(PickupPath);
	TestEqual(TEXT("the row is unchanged"), ReadValue(Pickup, TEXT("Pickup")).Contains(TEXT("Bost")), false);

	const FLevelToolResult Valid = Edit(*this, Set(false, FString::Printf(
		TEXT(R"({"Pickup": {"DataTable": "%s", "RowName": "Boost"}, "Key": "SpaceBar"})"), *Table.ObjectPath())), TEXT("valid"));
	TestTrue(TEXT("valid values are written"), Valid.bSuccess);
	Pickup = FindActorByPath(PickupPath);
	TestTrue(TEXT("row written"), ReadValue(Pickup, TEXT("Pickup")).Contains(TEXT("Boost")));
	TestEqual(TEXT("key written"), ReadValue(Pickup, TEXT("Key")), FString(TEXT("SpaceBar")));
	return true;
}

#endif
