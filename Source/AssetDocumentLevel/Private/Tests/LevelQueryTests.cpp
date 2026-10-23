// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// query_level on maps the tests create: a persistent level with one always-loaded
// sublevel, and a World Partition map whose far actors are unloaded after a reload.

#include "AssetDocumentLevelActorRef.h"
#include "Camera/CameraActor.h"
#include "Components/PointLightComponent.h"
#include "Editor.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "LevelEditTransaction.h"
#include "LevelQuery.h"
#include "LevelTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "WorldPartition/WorldPartition.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentLevelQueryTests
{
using namespace AssetDocumentLevelTests;
using UE::AssetDocumentLevel::Private::FLevelToolResult;
using UE::AssetDocumentLevel::Private::RunLevelQuery;

FLevelToolResult Query(const FString& Json)
{
	return RunLevelQuery(ParseJsonObject(Json));
}

TArray<TSharedPtr<FJsonValue>> Actors(const FLevelToolResult& Result)
{
	return Result.Payload.IsValid() && Result.Payload->HasTypedField<EJson::Array>(TEXT("actors"))
		? Result.Payload->GetArrayField(TEXT("actors")) : TArray<TSharedPtr<FJsonValue>>();
}

TArray<FString> Labels(const FLevelToolResult& Result)
{
	TArray<FString> Out;
	for (const TSharedPtr<FJsonValue>& Actor : Actors(Result))
	{
		Out.Add(Actor->AsObject()->GetStringField(TEXT("label")));
	}
	return Out;
}

TSharedPtr<FJsonObject> FindActor(const FLevelToolResult& Result, const FString& Label)
{
	for (const TSharedPtr<FJsonValue>& Actor : Actors(Result))
	{
		if (Actor->AsObject()->GetStringField(TEXT("label")) == Label)
		{
			return Actor->AsObject();
		}
	}
	return nullptr;
}

bool HasCode(const FLevelToolResult& Result, const TCHAR* Code, const TCHAR* Path = nullptr)
{
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (Diagnostic.Code == Code && (Path == nullptr || Diagnostic.Path == Path))
		{
			return true;
		}
	}
	return false;
}

TArray<FString> Sorted(TArray<FString> Values)
{
	Values.Sort();
	return Values;
}

// Persistent level "Room" + always-loaded sublevel "Sub".
struct FRoom
{
	UWorld* World = nullptr;
	ULevel* Sub = nullptr;
	FString RoomPackage;
	FString SubPackage;
	TWeakObjectPtr<AActor> KeyLight;
	TWeakObjectPtr<AActor> Floor;
	TWeakObjectPtr<AActor> Cam;
	TWeakObjectPtr<AActor> SubLight;
	TWeakObjectPtr<AActor> DupA;
	TWeakObjectPtr<AActor> DupB;
};

bool BuildRoom(FAutomationTestBase& Test, const FScopedTestMaps& Maps, FRoom& Room)
{
	Room.World = Maps.NewMap(TEXT("Room"), false);
	if (!Test.TestNotNull(TEXT("room map created"), Room.World))
	{
		return false;
	}
	Room.Sub = Maps.AddSublevel(*Room.World, TEXT("Sub"));
	if (!Test.TestNotNull(TEXT("sublevel created"), Room.Sub))
	{
		return false;
	}
	Room.RoomPackage = Maps.PackageName(TEXT("Room"));
	Room.SubPackage = Maps.PackageName(TEXT("Sub"));
	ULevel& Persistent = *Room.World->PersistentLevel;
	AActor* KeyLight = SpawnActor(Persistent, APointLight::StaticClass(), FVector(0, 0, 280), TEXT("KeyLight"));
	AActor* Floor = SpawnActor(Persistent, AStaticMeshActor::StaticClass(), FVector(0, 0, -10), TEXT("Floor"));
	AActor* Cam = SpawnActor(Persistent, ACameraActor::StaticClass(), FVector(-600, -600, 300), TEXT("Cam"));
	AActor* SubLight = SpawnActor(*Room.Sub, APointLight::StaticClass(), FVector(1000, 0, 280), TEXT("SubLight"));
	AActor* DupA = SpawnActor(Persistent, ATargetPoint::StaticClass(), FVector(200, 0, 0), TEXT("Dup"));
	AActor* DupB = SpawnActor(Persistent, ATargetPoint::StaticClass(), FVector(300, 0, 0), TEXT("Dup"));
	if (!Test.TestTrue(TEXT("actors spawned"), KeyLight && Floor && Cam && SubLight && DupA && DupB))
	{
		return false;
	}
	KeyLight->SetFolderPath(TEXT("Room"));
	KeyLight->Tags.Add(TEXT("Lighting"));
	Floor->SetFolderPath(TEXT("Room/Geometry"));
	Room.KeyLight = KeyLight;
	Room.Floor = Floor;
	Room.Cam = Cam;
	Room.SubLight = SubLight;
	Room.DupA = DupA;
	Room.DupB = DupB;
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelQueryWorldTest,
	"UEAssetDoc.Level.Query.World",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelQueryWorldTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelQueryTests;
	FScopedTestMaps Maps;
	FRoom Room;
	if (!BuildRoom(*this, Maps, Room))
	{
		return false;
	}
	const FLevelToolResult Result = Query(TEXT("{\"what\": \"world\"}"));
	if (!TestTrue(TEXT("world summary succeeds"), Result.bSuccess) || !Result.Payload.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>& World = Result.Payload;
	TestEqual(TEXT("world path"), World->GetStringField(TEXT("world")), Room.RoomPackage + TEXT(".Room"));
	TestFalse(TEXT("not partitioned"), World->GetBoolField(TEXT("partitioned")));
	TestEqual(TEXT("current level is the persistent level"), World->GetStringField(TEXT("current_level")), Room.RoomPackage);
	const TArray<TSharedPtr<FJsonValue>> Levels = World->GetArrayField(TEXT("levels"));
	if (TestEqual(TEXT("persistent level + one sublevel"), Levels.Num(), 2))
	{
		const TSharedPtr<FJsonObject> Persistent = Levels[0]->AsObject();
		const TSharedPtr<FJsonObject> Sub = Levels[1]->AsObject();
		TestEqual(TEXT("persistent package"), Persistent->GetStringField(TEXT("package")), Room.RoomPackage);
		TestTrue(TEXT("persistent flag"), Persistent->GetBoolField(TEXT("persistent")));
		TestEqual(TEXT("persistent listed actors"), static_cast<int32>(Persistent->GetNumberField(TEXT("actor_count"))), 5);
		TestEqual(TEXT("sublevel package"), Sub->GetStringField(TEXT("package")), Room.SubPackage);
		TestFalse(TEXT("sublevel is not persistent"), Sub->GetBoolField(TEXT("persistent")));
		TestTrue(TEXT("sublevel loaded"), Sub->GetBoolField(TEXT("loaded")));
		TestTrue(TEXT("sublevel visible"), Sub->GetBoolField(TEXT("visible")));
		TestFalse(TEXT("sublevel not locked"), Sub->GetBoolField(TEXT("locked")));
		TestEqual(TEXT("sublevel listed actors"), static_cast<int32>(Sub->GetNumberField(TEXT("actor_count"))), 1);
		TestEqual(TEXT("sublevel streaming class"), Sub->GetStringField(TEXT("streaming_class")),
			FString(TEXT("/Script/Engine.LevelStreamingAlwaysLoaded")));
	}
	TArray<FString> Folders;
	for (const TSharedPtr<FJsonValue>& Folder : World->GetArrayField(TEXT("folders")))
	{
		Folders.Add(Folder->AsString());
	}
	TestTrue(TEXT("folder Room listed"), Folders.Contains(TEXT("Room")));
	TestTrue(TEXT("folder Room/Geometry listed"), Folders.Contains(TEXT("Room/Geometry")));
	TestEqual(TEXT("listed actor count"), static_cast<int32>(World->GetNumberField(TEXT("actor_count"))), 6);
	TestEqual(TEXT("no unloaded actors"), static_cast<int32>(World->GetNumberField(TEXT("unloaded_actor_count"))), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelQueryFiltersTest,
	"UEAssetDoc.Level.Query.Filters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelQueryFiltersTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelQueryTests;
	FScopedTestMaps Maps;
	FRoom Room;
	if (!BuildRoom(*this, Maps, Room))
	{
		return false;
	}
	const TArray<FString> Lights = {TEXT("KeyLight"), TEXT("SubLight")};
	TestEqual(TEXT("class includes subclasses"), Sorted(Labels(Query(TEXT("{\"class\": \"/Script/Engine.Light\"}")))), Lights);
	TestEqual(TEXT("exact_class excludes subclasses"),
		Labels(Query(TEXT("{\"class\": \"/Script/Engine.Light\", \"exact_class\": true}"))).Num(), 0);
	TestEqual(TEXT("exact_class matches the class"),
		Sorted(Labels(Query(TEXT("{\"class\": \"/Script/Engine.PointLight\", \"exact_class\": true}")))), Lights);
	TestEqual(TEXT("name wildcard over labels"), Labels(Query(TEXT("{\"name\": \"Key*\"}"))), TArray<FString>{TEXT("KeyLight")});
	TestEqual(TEXT("name wildcard over object names"),
		Sorted(Labels(Query(TEXT("{\"name\": \"TargetPoint*\"}")))), TArray<FString>{TEXT("Dup"), TEXT("Dup")});
	TestEqual(TEXT("folder prefix includes subfolders"), Sorted(Labels(Query(TEXT("{\"folder\": \"Room\"}")))),
		TArray<FString>{TEXT("Floor"), TEXT("KeyLight")});
	TestEqual(TEXT("folder prefix matches whole segments"), Labels(Query(TEXT("{\"folder\": \"Roo\"}"))).Num(), 0);
	TestEqual(TEXT("tag"), Labels(Query(TEXT("{\"tag\": \"Lighting\"}"))), TArray<FString>{TEXT("KeyLight")});
	TestEqual(TEXT("level"), Labels(Query(FString::Printf(TEXT("{\"level\": \"%s\"}"), *Room.SubPackage))),
		TArray<FString>{TEXT("SubLight")});
	TestEqual(TEXT("near"), Labels(Query(TEXT("{\"near\": {\"location\": {\"X\": 0, \"Y\": 0, \"Z\": 270}, \"radius\": 20}}"))),
		TArray<FString>{TEXT("KeyLight")});
	TestEqual(TEXT("bounds"), Sorted(Labels(Query(
		TEXT("{\"bounds\": {\"min\": {\"X\": 150, \"Y\": -10, \"Z\": -10}, \"max\": {\"X\": 1100, \"Y\": 10, \"Z\": 300}}}")))),
		TArray<FString>{TEXT("Dup"), TEXT("Dup"), TEXT("SubLight")});

	GEditor->SelectNone(false, true);
	GEditor->SelectActor(Room.Cam.Get(), true, false);
	TestEqual(TEXT("selected_only"), Labels(Query(TEXT("{\"selected_only\": true}"))), TArray<FString>{TEXT("Cam")});
	GEditor->SelectNone(false, true);

	// Explicit actor references: path, guid, unique label.
	const FString KeyPath = UE::AssetDocumentLevel::GetActorPath(*Room.KeyLight);
	TestEqual(TEXT("the canonical path is the soft object path"), KeyPath,
		Room.RoomPackage + TEXT(".Room:PersistentLevel.") + Room.KeyLight->GetName());
	const FString Guid = UE::AssetDocumentLevel::FormatActorGuid(Room.KeyLight->GetActorGuid());
	const FLevelToolResult ByRefs = Query(FString::Printf(
		TEXT("{\"actors\": [\"%s\", {\"guid\": \"%s\"}, {\"label\": \"SubLight\"}, {\"label\": \"Cam\", \"level\": \"%s\"}]}"),
		*KeyPath, *Guid, *Room.RoomPackage));
	TestTrue(TEXT("actor references resolve"), ByRefs.bSuccess);
	TestEqual(TEXT("duplicate references are listed once, in level/name order"), Labels(ByRefs).Num(), 3);
	if (const TSharedPtr<FJsonObject> Key = FindActor(ByRefs, TEXT("KeyLight")))
	{
		TestEqual(TEXT("ActorRef path"), Key->GetStringField(TEXT("path")), KeyPath);
		TestEqual(TEXT("ActorRef guid"), Key->GetStringField(TEXT("guid")), Guid);
		TestEqual(TEXT("ActorRef class"), Key->GetStringField(TEXT("class")), FString(TEXT("/Script/Engine.PointLight")));
		TestEqual(TEXT("ActorRef level"), Key->GetStringField(TEXT("level")), Room.RoomPackage);
		TestTrue(TEXT("ActorRef external_package is null"), Key->HasTypedField<EJson::Null>(TEXT("external_package")));
		TestTrue(TEXT("ActorRef loaded"), Key->GetBoolField(TEXT("loaded")));
		TestEqual(TEXT("folder"), Key->GetStringField(TEXT("folder")), FString(TEXT("Room")));
	}
	// Filters apply to explicit references too.
	TestEqual(TEXT("references are filtered"), Labels(Query(FString::Printf(
		TEXT("{\"actors\": [\"%s\", {\"label\": \"Cam\"}], \"class\": \"/Script/Engine.Light\"}"), *KeyPath))),
		TArray<FString>{TEXT("KeyLight")});

	const FLevelToolResult Ambiguous = Query(TEXT("{\"actors\": [{\"label\": \"Dup\"}]}"));
	TestFalse(TEXT("an ambiguous label fails"), Ambiguous.bSuccess);
	TestTrue(TEXT("Level.ActorRefAmbiguous"), HasCode(Ambiguous, TEXT("Level.ActorRefAmbiguous"), TEXT("/actors/0")));
	if (const FAssetDocumentDiagnostic* Diagnostic = Ambiguous.FindDiagnostic(TEXT("Level.ActorRefAmbiguous")))
	{
		TestTrue(TEXT("the candidates are listed"), Diagnostic->Details.IsValid()
			&& Diagnostic->Details->GetArrayField(TEXT("candidates")).Num() == 2);
	}
	TestTrue(TEXT("unknown path"), HasCode(Query(TEXT("{\"actors\": [\"/Game/Nope.Nope:PersistentLevel.Nope\"]}")),
		TEXT("Level.ActorNotFound"), TEXT("/actors/0")));
	TestTrue(TEXT("batch ids are refused in queries"), HasCode(Query(TEXT("{\"actors\": [\"@door\"]}")),
		TEXT("Level.InvalidRequest"), TEXT("/actors/0")));
	TestTrue(TEXT("unknown field"), HasCode(Query(TEXT("{\"bogus\": 1}")), TEXT("Level.InvalidRequest"), TEXT("/bogus")));
	TestTrue(TEXT("limit range"), HasCode(Query(TEXT("{\"limit\": 0}")), TEXT("Level.InvalidRequest"), TEXT("/limit")));
	TestTrue(TEXT("limit maximum"), HasCode(Query(TEXT("{\"limit\": 1001}")), TEXT("Level.InvalidRequest"), TEXT("/limit")));
	TestTrue(TEXT("unknown class"), HasCode(Query(TEXT("{\"class\": \"/Script/Engine.NoSuchActor\"}")),
		TEXT("Level.InvalidRequest"), TEXT("/class")));
	TestTrue(TEXT("detail value"), HasCode(Query(TEXT("{\"detail\": \"all\"}")), TEXT("Level.InvalidRequest"), TEXT("/detail")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelQueryPagingTest,
	"UEAssetDoc.Level.Query.Paging",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelQueryPagingTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelQueryTests;
	FScopedTestMaps Maps;
	FRoom Room;
	if (!BuildRoom(*this, Maps, Room))
	{
		return false;
	}
	for (int32 Index = 0; Index < 7; ++Index)
	{
		SpawnActor(Index % 2 == 0 ? *Room.World->PersistentLevel : *Room.Sub, ATargetPoint::StaticClass(),
			FVector(Index * 100.0, 500, 0), *FString::Printf(TEXT("P%d"), Index));
	}
	const FLevelToolResult All = Query(TEXT("{\"name\": \"P?\", \"limit\": 1000}"));
	TestEqual(TEXT("all seven"), Labels(All).Num(), 7);
	TArray<FString> AllPaths;
	TArray<FString> Order;
	for (const TSharedPtr<FJsonValue>& Actor : Actors(All))
	{
		const TSharedPtr<FJsonObject> Object = Actor->AsObject();
		AllPaths.Add(Object->GetStringField(TEXT("path")));
		Order.Add(Object->GetStringField(TEXT("level")) + TEXT("|") + FPaths::GetExtension(Object->GetStringField(TEXT("path"))));
	}
	TArray<FString> Expected = Order;
	Expected.Sort([](const FString& Left, const FString& Right) { return Left.Compare(Right, ESearchCase::CaseSensitive) < 0; });
	TestEqual(TEXT("ordered by level, then object name"), Order, Expected);

	TArray<FString> Paged;
	for (int32 Offset = 0; Offset < 9; Offset += 3)
	{
		const FLevelToolResult Page = Query(FString::Printf(TEXT("{\"name\": \"P?\", \"limit\": 3, \"offset\": %d}"), Offset));
		TestEqual(TEXT("total is the full count"), static_cast<int32>(Page.Payload->GetNumberField(TEXT("total"))), 7);
		for (const TSharedPtr<FJsonValue>& Actor : Actors(Page))
		{
			Paged.Add(Actor->AsObject()->GetStringField(TEXT("path")));
		}
	}
	TestEqual(TEXT("pages concatenate to the full ordered list"), Paged, AllPaths);
	TestEqual(TEXT("an offset past the end is empty"),
		Labels(Query(TEXT("{\"name\": \"P?\", \"offset\": 50}"))).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelQueryDetailTest,
	"UEAssetDoc.Level.Query.Detail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelQueryDetailTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelQueryTests;
	FScopedTestMaps Maps;
	FRoom Room;
	if (!BuildRoom(*this, Maps, Room))
	{
		return false;
	}
	UPointLightComponent* Light = Cast<APointLight>(Room.KeyLight.Get())->PointLightComponent;
	Light->SetIntensity(8000.0f);

	const FLevelToolResult Summary = Query(TEXT("{\"name\": \"KeyLight\"}"));
	const TSharedPtr<FJsonObject> Key = FindActor(Summary, TEXT("KeyLight"));
	if (!TestNotNull(TEXT("summary lists the light"), Key.Get()))
	{
		return false;
	}
	TestFalse(TEXT("summary has no components"), Key->HasField(TEXT("components")));
	const TSharedPtr<FJsonObject> Transform = Key->GetObjectField(TEXT("transform"));
	TestEqual(TEXT("transform location Z"), Transform->GetObjectField(TEXT("location"))->GetNumberField(TEXT("Z")), 280.0);
	TestTrue(TEXT("rotation is a rotator"), Transform->GetObjectField(TEXT("rotation"))->HasField(TEXT("Yaw")));
	TestEqual(TEXT("scale X"), Transform->GetObjectField(TEXT("scale"))->GetNumberField(TEXT("X")), 1.0);
	TestEqual(TEXT("tags"), Key->GetArrayField(TEXT("tags")).Num(), 1);

	const FLevelToolResult WithComponents = Query(TEXT("{\"name\": \"KeyLight\", \"detail\": \"components\"}"));
	const TSharedPtr<FJsonObject> KeyComponents = FindActor(WithComponents, TEXT("KeyLight"));
	TSharedPtr<FJsonObject> LightComponent;
	for (const TSharedPtr<FJsonValue>& Component : KeyComponents->GetArrayField(TEXT("components")))
	{
		if (Component->AsObject()->GetStringField(TEXT("name")) == Light->GetName())
		{
			LightComponent = Component->AsObject();
		}
	}
	if (!TestNotNull(TEXT("the light component is listed by name"), LightComponent.Get()))
	{
		return false;
	}
	TestEqual(TEXT("component class"), LightComponent->GetStringField(TEXT("class")), FString(TEXT("/Script/Engine.PointLightComponent")));
	TestEqual(TEXT("native creation"), LightComponent->GetStringField(TEXT("creation")), FString(TEXT("Native")));
	TestTrue(TEXT("editable like the Details panel"), LightComponent->GetBoolField(TEXT("editable")));
	TestTrue(TEXT("root component"), LightComponent->GetBoolField(TEXT("root")));
	TestFalse(TEXT("components detail has no properties"), LightComponent->HasField(TEXT("properties")));
	TestEqual(TEXT("the root component is listed first"),
		KeyComponents->GetArrayField(TEXT("components"))[0]->AsObject()->GetStringField(TEXT("name")), Light->GetName());

	const FLevelToolResult WithProperties = Query(TEXT("{\"name\": \"KeyLight\", \"detail\": \"properties\"}"));
	const TSharedPtr<FJsonObject> KeyProperties = FindActor(WithProperties, TEXT("KeyLight"));
	TSharedPtr<FJsonObject> LightProperties;
	for (const TSharedPtr<FJsonValue>& Component : KeyProperties->GetArrayField(TEXT("components")))
	{
		if (Component->AsObject()->GetStringField(TEXT("name")) == Light->GetName())
		{
			LightProperties = Component->AsObject()->GetObjectField(TEXT("properties"));
		}
	}
	if (!TestNotNull(TEXT("component properties"), LightProperties.Get()))
	{
		return false;
	}
	TestEqual(TEXT("the instance override is listed"), LightProperties->GetNumberField(TEXT("Intensity")), 8000.0);
	TestFalse(TEXT("diff_only leaves out archetype values"), LightProperties->HasField(TEXT("AttenuationRadius")));
	const TSharedPtr<FJsonObject> ActorProperties = KeyProperties->GetObjectField(TEXT("properties"));
	TestTrue(TEXT("actor overrides: Tags"), ActorProperties->HasField(TEXT("Tags")));
	TestFalse(TEXT("component pointers are not actor properties"), ActorProperties->HasField(TEXT("PointLightComponent")));

	const FLevelToolResult Full = Query(TEXT("{\"name\": \"KeyLight\", \"detail\": \"properties\", \"diff_only\": false}"));
	for (const TSharedPtr<FJsonValue>& Component : FindActor(Full, TEXT("KeyLight"))->GetArrayField(TEXT("components")))
	{
		if (Component->AsObject()->GetStringField(TEXT("name")) == Light->GetName())
		{
			TestTrue(TEXT("diff_only false lists every writable property"),
				Component->AsObject()->GetObjectField(TEXT("properties"))->HasField(TEXT("AttenuationRadius")));
		}
	}
	return true;
}

// query_level and query_play share their conventions: actors by Outliner label or object name
// as well as by path, properties: [...] value paths, detail "full" (= "properties").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelQuerySharedConventionsTest,
	"UEAssetDoc.Level.Query.SharedConventions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelQuerySharedConventionsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelQueryTests;
	FScopedTestMaps Maps;
	FRoom Room;
	if (!BuildRoom(*this, Maps, Room))
	{
		return false;
	}
	UPointLightComponent* Light = Cast<APointLight>(Room.KeyLight.Get())->PointLightComponent;
	Light->SetIntensity(8000.0f);

	// A bare label, a bare object name.
	const FLevelToolResult ByLabel = Query(FString::Printf(TEXT("{\"actors\": [\"KeyLight\", \"%s\"]}"), *Room.Floor->GetName()));
	TestTrue(*FString::Printf(TEXT("a label and an object name resolve (%s)"), *ByLabel.Message), ByLabel.bSuccess);
	TestEqual(TEXT("both actors"), Sorted(Labels(ByLabel)), Sorted(TArray<FString>{TEXT("KeyLight"), TEXT("Floor")}));
	const FLevelToolResult Ambiguous = Query(TEXT("{\"actors\": [\"Dup\"]}"));
	TestTrue(TEXT("a bare label shared by two actors is ambiguous"), HasCode(Ambiguous, TEXT("Level.ActorRefAmbiguous"), TEXT("/actors/0")));
	const FLevelToolResult Unknown = Query(TEXT("{\"actors\": [\"KeyLite\"]}"));
	TestTrue(TEXT("an unknown bare name"), HasCode(Unknown, TEXT("Level.ActorNotFound"), TEXT("/actors/0")));
	if (const FAssetDocumentDiagnostic* Diagnostic = Unknown.FindDiagnostic(TEXT("Level.ActorNotFound")))
	{
		TestTrue(TEXT("the message names label and object name"), Diagnostic->Message.Contains(TEXT("labelled or named")));
		TestTrue(TEXT("and the close label"), Diagnostic->Message.Contains(TEXT("KeyLight")));
	}

	// properties: value paths as query_play reads them.
	const FLevelToolResult Values = Query(TEXT("{\"actors\": [\"KeyLight\"], \"properties\": [\"PointLightComponent.Intensity\", "
		"\"RootComponent.RelativeLocation.Z\", \"NoSuchValue\"]}"));
	const TSharedPtr<FJsonObject> Key = FindActor(Values, TEXT("KeyLight"));
	if (TestNotNull(*FString::Printf(TEXT("properties query (%s)"), *Values.Message), Key.Get()))
	{
		const TSharedPtr<FJsonObject>* Read = nullptr;
		if (TestTrue(TEXT("values"), Key->TryGetObjectField(TEXT("values"), Read)))
		{
			TestEqual(TEXT("a component value"), (*Read)->GetNumberField(TEXT("PointLightComponent.Intensity")), 8000.0);
			TestEqual(TEXT("a struct member"), (*Read)->GetNumberField(TEXT("RootComponent.RelativeLocation.Z")), 280.0);
		}
		TestTrue(TEXT("an unknown path is reported"), Key->HasField(TEXT("value_errors")));
	}
	TestTrue(TEXT("properties must be strings"), HasCode(Query(TEXT("{\"properties\": [1]}")), TEXT("Level.InvalidRequest"), TEXT("/properties")));

	// detail "full" is "properties".
	const TSharedPtr<FJsonObject> Full = FindActor(Query(TEXT("{\"actors\": [\"KeyLight\"], \"detail\": \"full\"}")), TEXT("KeyLight"));
	TestTrue(TEXT("detail full lists components and properties"), Full.IsValid() && Full->HasField(TEXT("components")) && Full->HasField(TEXT("properties")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelQueryWorldPartitionUnloadedTest,
	"UEAssetDoc.Level.Query.WorldPartitionUnloaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelQueryWorldPartitionUnloadedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelQueryTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Open"), true);
	if (!TestNotNull(TEXT("partitioned map created"), World) || !TestTrue(TEXT("partitioned"), World->IsPartitionedWorld()))
	{
		return false;
	}
	AActor* Near = SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(0, 0, 100), TEXT("Near"));
	AActor* Far = SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(400000, 400000, 100), TEXT("Far"));
	AActor* Always = SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(-400000, 0, 100), TEXT("Always"));
	if (!TestTrue(TEXT("actors spawned"), Near && Far && Always))
	{
		return false;
	}
	Always->SetIsSpatiallyLoaded(false);
	TestNotNull(TEXT("OFPA: actors have their own package"), Far->GetExternalPackage());
	const FString FarPath = UE::AssetDocumentLevel::GetActorPath(*Far);
	const FGuid FarGuidValue = Far->GetActorGuid();
	const FString FarGuid = UE::AssetDocumentLevel::FormatActorGuid(FarGuidValue);
	const FString FarPackage = Far->GetExternalPackage() != nullptr ? Far->GetExternalPackage()->GetName() : FString();

	const FLevelToolResult Loaded = Query(TEXT("{\"class\": \"/Script/Engine.PointLight\"}"));
	if (const TSharedPtr<FJsonObject> FarLoaded = FindActor(Loaded, TEXT("Far")))
	{
		TestEqual(TEXT("external_package of a loaded OFPA actor"), FarLoaded->GetStringField(TEXT("external_package")), FarPackage);
	}

	if (!TestTrue(TEXT("map saved"), FScopedTestMaps::SaveWorld(*World)))
	{
		return false;
	}
	World = Maps.LoadMap(TEXT("Open"));
	if (!TestNotNull(TEXT("map reloaded"), World))
	{
		return false;
	}

	const FLevelToolResult LoadedOnly = Query(TEXT("{\"class\": \"/Script/Engine.PointLight\"}"));
	const TArray<FString> LoadedLabels = Labels(LoadedOnly);
	TestTrue(TEXT("the non-spatially-loaded actor is loaded"), LoadedLabels.Contains(TEXT("Always")));
	TestFalse(TEXT("the far actor is not loaded after the reload"), LoadedLabels.Contains(TEXT("Far")));

	const FLevelToolResult WithUnloaded = Query(TEXT("{\"class\": \"/Script/Engine.PointLight\", \"include_unloaded\": true}"));
	const TSharedPtr<FJsonObject> FarEntry = FindActor(WithUnloaded, TEXT("Far"));
	if (TestNotNull(TEXT("include_unloaded lists the far actor from its descriptor"), FarEntry.Get()))
	{
		TestFalse(TEXT("loaded: false"), FarEntry->GetBoolField(TEXT("loaded")));
		TestEqual(TEXT("descriptor path is the canonical path"), FarEntry->GetStringField(TEXT("path")), FarPath);
		TestEqual(TEXT("descriptor guid"), FarEntry->GetStringField(TEXT("guid")), FarGuid);
		TestEqual(TEXT("descriptor class"), FarEntry->GetStringField(TEXT("class")), FString(TEXT("/Script/Engine.PointLight")));
		TestEqual(TEXT("descriptor package"), FarEntry->GetStringField(TEXT("external_package")), FarPackage);
		TestTrue(TEXT("descriptor bounds"), FarEntry->HasTypedField<EJson::Object>(TEXT("bounds")));
		TestFalse(TEXT("no transform for an unloaded actor"), FarEntry->HasField(TEXT("transform")));
	}
	TestTrue(TEXT("the near-by filter applies to descriptors"), Labels(Query(TEXT(
		"{\"include_unloaded\": true, \"near\": {\"location\": {\"X\": 400000, \"Y\": 400000, \"Z\": 100}, \"radius\": 10}}")))
		.Contains(TEXT("Far")));

	const FLevelToolResult ByPath = Query(FString::Printf(TEXT("{\"actors\": [\"%s\"]}"), *FarPath));
	TestTrue(TEXT("an unloaded actor resolves by path"), ByPath.bSuccess && Labels(ByPath) == TArray<FString>{TEXT("Far")});
	const FLevelToolResult ByGuid = Query(FString::Printf(TEXT("{\"actors\": [{\"guid\": \"%s\"}]}"), *FarGuid));
	TestTrue(TEXT("an unloaded actor resolves by guid"), ByGuid.bSuccess && Labels(ByGuid) == TArray<FString>{TEXT("Far")});

	const FLevelToolResult Summary = Query(TEXT("{\"what\": \"world\"}"));
	TestTrue(TEXT("world summary: partitioned"), Summary.Payload->GetBoolField(TEXT("partitioned")));
	TestTrue(TEXT("world summary counts unloaded actors"), Summary.Payload->GetNumberField(TEXT("unloaded_actor_count")) >= 1.0);

	// edit_level: an unloaded actor is not editable (Level.ActorNotLoaded); pin loads it
	// (outside the Undo history) and it is editable from the next request on; unpin is
	// refused while it has unsaved changes, and unloads it once saved.
	using UE::AssetDocumentLevel::Private::RunLevelEdit;
	auto Edit = [](const FString& Json) { return RunLevelEdit(ParseJsonObject(Json)); };
	const FLevelToolResult NotLoaded = Edit(FString::Printf(TEXT(R"({"ops": [{"op": "set", "actor": "%s", "label": "FarAway"}]})"), *FarPath));
	TestTrue(TEXT("editing an unloaded actor: Level.ActorNotLoaded"), HasCode(NotLoaded, TEXT("Level.ActorNotLoaded"), TEXT("/ops/0/actor")));
	const FLevelToolResult Pinned = Edit(FString::Printf(TEXT(R"({"ops": [{"op": "pin", "actors": [{"guid": "%s"}]}]})"), *FarGuid));
	const TArray<TSharedPtr<FJsonValue>>* PinnedPaths = nullptr;
	TestTrue(TEXT("pin succeeds and reports the actor"), Pinned.bSuccess && Pinned.Payload->TryGetArrayField(TEXT("pinned"), PinnedPaths)
		&& PinnedPaths->Num() == 1 && (*PinnedPaths)[0]->AsString() == FarPath);
	TestTrue(TEXT("the pinned actor is loaded"), Labels(Query(TEXT("{\"class\": \"/Script/Engine.PointLight\"}"))).Contains(TEXT("Far")));
	const FLevelToolResult Renamed = Edit(FString::Printf(TEXT(R"({"ops": [{"op": "set", "actor": "%s", "label": "FarAway"}]})"), *FarPath));
	TestTrue(TEXT("the pinned actor is editable"), Renamed.bSuccess);
	const FLevelToolResult Dirty = Edit(FString::Printf(TEXT(R"({"ops": [{"op": "unpin", "actors": ["%s"]}]})"), *FarPath));
	TestTrue(TEXT("unpin of an actor with unsaved changes is refused"), HasCode(Dirty, TEXT("Level.InvalidRequest"), TEXT("/ops/0/actors/0")));
	const FLevelToolResult Saved = Edit(FString::Printf(TEXT(R"({"save": true, "ops": [{"op": "set", "actor": "%s", "tags": ["far"]}]})"),
		*FarPath));
	TestTrue(TEXT("saved"), Saved.bSuccess && Saved.Payload->GetArrayField(TEXT("saved_packages")).Num() == 1);
	const FLevelToolResult Unpinned = Edit(FString::Printf(TEXT(R"({"ops": [{"op": "unpin", "actors": ["%s"]}]})"), *FarPath));
	TestTrue(TEXT("unpin succeeds once saved"), Unpinned.bSuccess && Unpinned.Payload->GetArrayField(TEXT("unpinned")).Num() == 1);
	// Unloading can make the engine reset the Undo history; the response then says so.
	const bool bCleared = HasCode(Unpinned, TEXT("Level.UndoHistoryCleared"));
	AddInfo(FString::Printf(TEXT("unpin cleared the Undo history: %s"), bCleared ? TEXT("yes") : TEXT("no")));
	if (bCleared)
	{
		TestFalse(TEXT("not undoable after the history was cleared"),
			Unpinned.Payload->GetObjectField(TEXT("transaction"))->GetBoolField(TEXT("undoable")));
	}
	UWorldPartition* Partition = GetEditorWorld() != nullptr ? GetEditorWorld()->GetWorldPartition() : nullptr;
	TestTrue(TEXT("the far actor is no longer pinned"), Partition != nullptr && !Partition->IsActorPinned(FarGuidValue));
	AddInfo(FString::Printf(TEXT("after unpin the far actor is %s"), Labels(Query(TEXT("{\"class\": \"/Script/Engine.PointLight\"}")))
		.Contains(TEXT("FarAway")) ? TEXT("still loaded") : TEXT("unloaded")));
	const FLevelToolResult NotPartitioned = Edit(TEXT(R"({"ops": [{"op": "pin", "actors": ["@x"]}]})"));
	TestTrue(TEXT("pin takes actors that exist before the request"), HasCode(NotPartitioned, TEXT("Level.InvalidRequest"), TEXT("/ops/0/actors/0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelQueryPlaySessionTest,
	"UEAssetDoc.Level.Query.PlaySession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelQueryPlaySessionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelQueryTests;
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	TestTrue(TEXT("query works outside a play session"), Query(TEXT("{}")).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	const double Started = FPlatformTime::Seconds();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Started]()
	{
		if (GEditor->PlayWorld == nullptr)
		{
			if (FPlatformTime::Seconds() - Started > 60.0)
			{
				AddError(TEXT("the play session did not start"));
				return true;
			}
			return false;
		}
		const FLevelToolResult Refused = Query(TEXT("{}"));
		TestFalse(TEXT("query is refused during a play session"), Refused.bSuccess);
		TestEqual(TEXT("HTTP 400"), Refused.HttpStatus, 400);
		TestTrue(TEXT("Level.PlaySessionActive"), HasCode(Refused, TEXT("Level.PlaySessionActive")));
		TestFalse(TEXT("nothing is returned"), Refused.Payload.IsValid());
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Started]()
	{
		if (GEditor->PlayWorld != nullptr && FPlatformTime::Seconds() - Started < 120.0)
		{
			return false;
		}
		TestNull(TEXT("the play session ended"), GEditor->PlayWorld.Get());
		TestTrue(TEXT("query works again"), Query(TEXT("{}")).bSuccess);
		return true;
	}));
	return true;
}
// There was no way to tell whether the open map had unsaved changes before open_level
// refused (Level.UnsavedChangesWouldBeLost). The world summary lists the dirty level packages.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelQueryWorldUnsavedChangesTest,
	"UEAssetDoc.Level.Query.WorldUnsavedChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelQueryWorldUnsavedChangesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelQueryTests;
	using UE::AssetDocumentLevel::Private::RunLevelEdit;
	FScopedTestMaps Maps;
	if (!TestNotNull(TEXT("map created"), Maps.NewMap(TEXT("Unsaved"), false)))
	{
		return false;
	}
	const FString MapPackage = Maps.PackageName(TEXT("Unsaved"));
	auto DirtyPackages = [this](const TCHAR* Stage, bool& bNeverSaved)
	{
		TArray<FString> Out;
		const FLevelToolResult Result = Query(TEXT("{\"what\": \"world\"}"));
		if (TestTrue(*FString::Printf(TEXT("%s: world summary succeeds"), Stage), Result.bSuccess && Result.Payload.IsValid())
			&& TestTrue(*FString::Printf(TEXT("%s: dirty_packages listed"), Stage), Result.Payload->HasTypedField<EJson::Array>(TEXT("dirty_packages"))))
		{
			for (const TSharedPtr<FJsonValue>& Value : Result.Payload->GetArrayField(TEXT("dirty_packages")))
			{
				Out.Add(Value->AsString());
			}
			bNeverSaved = Result.Payload->GetBoolField(TEXT("never_saved"));
		}
		return Out;
	};
	bool bNeverSaved = false;
	const FLevelToolResult Saved = RunLevelEdit(ParseJsonObject(TEXT(R"({"description": "AI: build", "save": true,
		"ops": [{"op": "spawn", "class": "/Script/Engine.PointLight", "label": "Lamp"}]})")));
	TestTrue(TEXT("built and saved"), Saved.bSuccess);
	TestEqual(TEXT("saved: no unsaved changes"), DirtyPackages(TEXT("saved"), bNeverSaved), TArray<FString>());
	TestFalse(TEXT("saved: the map is on disk"), bNeverSaved);
	const FLevelToolResult Moved = RunLevelEdit(ParseJsonObject(TEXT(R"({"description": "AI: move",
		"ops": [{"op": "set", "actor": {"label": "Lamp"}, "transform": {"location": {"X": 50}}}]})")));
	TestTrue(TEXT("moved without saving"), Moved.bSuccess);
	TestEqual(TEXT("unsaved: the map is listed"), DirtyPackages(TEXT("unsaved"), bNeverSaved), TArray<FString>{MapPackage});
	return true;
}
#endif
