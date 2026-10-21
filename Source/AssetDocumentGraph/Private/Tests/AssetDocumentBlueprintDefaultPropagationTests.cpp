// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// After a Blueprint apply the live dependents look exactly as after the same edit in the
// Blueprint editor (AssetDocumentGraphDefaultsPropagation.h), and an omitted or empty region means
// what the editor state it describes means.
//
// Blueprint.DefaultPropagation.ParentToChildAndInstances (a new variable default, an
// inherited HitFX-like value and an inherited Mesh AnimClass that never reached the child Blueprint
// or the placed actors): parent BP_P (Health, Speed, SCS Mesh), child BP_C (Speed and an inherited
// Mesh override), one placed actor of each in an editor level (P0 overrides Health, C0 overrides
// InitialLifeSpan and its Mesh AnimClass; Health is Instance Editable: a placed actor can only
// override such a variable, and a compile resets the others to the class default). An apply of BP_P adds Armor, changes Health, the class
// default InitialLifeSpan and Mesh AnimClass: the child class defaults, the child's Mesh override
// and both actors hold the new values wherever they held the old ones; every override stays; the
// child Blueprint stays saved and extracts unchanged.
// Blueprint.DefaultPropagation.InheritedNullOverride: Base > Mid (Mesh StaticMesh = Cube) >
// Leaf (Mesh StaticMesh = null): the null override is written, extracted, a no-op when re-applied,
// and survives save + reload.
// Blueprint.EmptyEventGraph: "UbergraphPages": [] empties the EventGraph (it cannot be deleted)
// in the same apply that removes the variable and component its nodes use.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_Event.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintDefaultPropagationTests
{
const TCHAR* Cube = TEXT("/Engine/BasicShapes/Cube.Cube");

struct FPropagationAsset
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString ClassPath;
	FString PackageFile;
	FString SidecarPath;

	FPropagationAsset(const FString& Folder, const TCHAR* InName)
		: Name(InName)
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		ClassPath = ObjectPath + TEXT("_C");
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
	}

	UBlueprint* Get() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }
	UClass* Generated() const { const UBlueprint* Blueprint = Get(); return Blueprint ? Blueprint->GeneratedClass.Get() : nullptr; }
	UObject* Defaults() const { UClass* Class = Generated(); return Class ? Class->GetDefaultObject() : nullptr; }
	FString Document(const TCHAR* Action, const FString& Body) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",\"Body\":{%s}}"),
			*PackageName, Action, *Body);
	}
};

// Blueprints in a unique folder under /Game/AssetDocumentP66, unloaded and deleted at the end.
struct FPropagationFixture
{
	FAutomationTestBase& Test;
	const FString Folder;
	TArray<FPropagationAsset> Assets;

	FPropagationFixture(FAutomationTestBase& InTest, std::initializer_list<const TCHAR*> Names)
		: Test(InTest)
		, Folder(FString(TEXT("AssetDocumentP66/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits))
	{
		for (const TCHAR* Name : Names)
		{
			Assets.Emplace(Folder, Name);
		}
	}

	~FPropagationFixture()
	{
		Unload();
		TArray<FString> Files;
		for (const FPropagationAsset& Asset : Assets)
		{
			IFileManager::Get().Delete(*Asset.PackageFile, false, true);
			IFileManager::Get().Delete(*Asset.SidecarPath, false, true);
			Files.Add(Asset.PackageFile);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		if (!Assets.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Assets[0].SidecarPath), false, true);
		}
	}

	const FPropagationAsset& operator[](int32 Index) const { return Assets[Index]; }

	// Unloads every fixture package (children first), as an editor restart would before a load.
	void Unload() const
	{
		TArray<UPackage*> Packages;
		for (int32 Index = Assets.Num() - 1; Index >= 0; --Index)
		{
			if (UPackage* Package = FindPackage(nullptr, *Assets[Index].PackageName))
			{
				Package->SetDirtyFlag(false);
				Packages.Add(Package);
			}
		}
		if (!Packages.IsEmpty())
		{
			UPackageTools::UnloadPackages(Packages);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	FAssetDocumentResult Apply(const FPropagationAsset& Asset, const FString& Json, const FString& Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset.SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *Asset.SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Asset.SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
		return Result;
	}

	bool ApplyOk(const FPropagationAsset& Asset, const FString& Json, const FString& Stage) const
	{
		return Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Apply(Asset, Json, Stage).IsSuccess());
	}

	TSharedPtr<FJsonObject> ExtractBody(const FPropagationAsset& Asset, const FString& Stage) const
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Asset.ObjectPath;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
		const TSharedPtr<FJsonObject>* Body = nullptr;
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract %s"), *Stage, *Asset.Name), Result.IsSuccess() && Result.Payload.IsValid()
			&& Result.Payload->TryGetObjectField(TEXT("Body"), Body)))
		{
			return nullptr;
		}
		return *Body;
	}
};

FString Json(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

// The Properties of the extracted component Name ("" when it has none, "<missing>" without the component).
FString ExtractedComponentProperties(const TSharedPtr<FJsonObject>& Body, const TCHAR* Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	if (Body.IsValid() && Body->TryGetArrayField(TEXT("Components"), Components))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Components)
		{
			const TSharedPtr<FJsonObject> Component = Value->AsObject();
			if (Component.IsValid() && Component->GetStringField(TEXT("Name")) == Name)
			{
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				return Component->TryGetObjectField(TEXT("Properties"), Properties) ? Json(*Properties) : FString();
			}
		}
	}
	return TEXT("<missing>");
}

double PropagationDouble(const UObject* Object, const TCHAR* Name)
{
	const FDoubleProperty* Property = Object ? FindFProperty<FDoubleProperty>(Object->GetClass(), Name) : nullptr;
	return Property ? Property->GetPropertyValue_InContainer(Object) : -1.0;
}

void SetPropagationDouble(UObject* Object, const TCHAR* Name, double Value)
{
	if (const FDoubleProperty* Property = Object ? FindFProperty<FDoubleProperty>(Object->GetClass(), Name) : nullptr)
	{
		Property->SetPropertyValue_InContainer(Object, Value);
	}
}

// The component template of a child Blueprint's inherited override (its ICH record).
template <typename TComponent>
TComponent* InheritedTemplate(UBlueprint* Child, const FName ComponentName)
{
	UInheritableComponentHandler* Handler = Child ? Child->GetInheritableComponentHandler(false) : nullptr;
	if (Handler == nullptr)
	{
		return nullptr;
	}
	for (auto It = Handler->CreateRecordIterator(); It; ++It)
	{
		if (It->ComponentKey.GetSCSVariableName() == ComponentName)
		{
			return Cast<TComponent>(It->ComponentTemplate);
		}
	}
	return nullptr;
}

// The one valid actor of exactly this class in the level (placed actors are replaced by the
// reinstancing of a compile, so they are looked up again after every apply).
AActor* PlacedActorOf(const UWorld* World, const UClass* Class)
{
	AActor* Found = nullptr;
	for (AActor* Actor : World && World->PersistentLevel ? World->PersistentLevel->Actors : TArray<TObjectPtr<AActor>>())
	{
		if (IsValid(Actor) && Class != nullptr && Actor->GetClass() == Class)
		{
			Found = Actor;
		}
	}
	return Found;
}

USkeletalMeshComponent* MeshOf(const AActor* Actor)
{
	if (Actor == nullptr)
	{
		return nullptr;
	}
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(Actor);
	return Meshes.Num() == 1 ? Meshes[0] : nullptr;
}

UClass* AnimClassOf(const USkeletalMeshComponent* Mesh)
{
	return Mesh ? Mesh->AnimClass.Get() : nullptr;
}
} // namespace AssetDocumentBlueprintDefaultPropagationTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintDefaultPropagationTest,
	"UEAssetDoc.Blueprint.DefaultPropagation.ParentToChildAndInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintDefaultPropagationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDefaultPropagationTests;
	const FPropagationFixture Fixture(*this, {TEXT("BP_P66Parent"), TEXT("BP_P66Child")});
	const FPropagationAsset& Parent = Fixture[0];
	const FPropagationAsset& Child = Fixture[1];

	auto ParentBody = [](const TCHAR* Health, const TCHAR* Extra)
	{
		return FString::Printf(TEXT("\"ParentClass\":\"/Script/Engine.Actor\","
			"\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\",\"InstanceEditable\":true,\"DefaultValue\":%s},{\"Name\":\"Speed\",\"Type\":\"float\",\"DefaultValue\":10.0}%s"),
			Health, Extra);
	};
	const FString ParentCreate = Parent.Document(TEXT("Create"), ParentBody(TEXT("100.0"), TEXT("],"
		"\"Components\":[{\"Name\":\"Mesh\",\"Class\":\"/Script/Engine.SkeletalMeshComponent\"}]")));
	const FString ChildCreate = Child.Document(TEXT("Create"), FString::Printf(TEXT("\"ParentClass\":\"%s\",\"ClassDefaults\":{\"Speed\":20.0},"
		"\"Components\":[{\"Name\":\"Mesh\",\"Scope\":\"Inherited\",\"Properties\":{\"bHiddenInGame\":true}}]"), *Parent.ClassPath));
	if (!Fixture.ApplyOk(Parent, ParentCreate, TEXT("create parent")) || !Fixture.ApplyOk(Child, ChildCreate, TEXT("create child")))
	{
		return false;
	}

	// A loaded editor level with one placed actor of each Blueprint and their instance overrides.
	const UWorld::InitializationValues Values = UWorld::InitializationValues().CreatePhysicsScene(false).ShouldSimulatePhysics(false)
		.CreateNavigation(false).CreateAISystem(false).AllowAudioPlayback(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("AssetDocumentP66Level"), nullptr, true, ERHIFeatureLevel::Num, &Values);
	if (!TestNotNull(TEXT("editor level"), World))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		World->RemoveFromRoot();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	};
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* P0 = World->SpawnActor<AActor>(Parent.Generated(), FTransform::Identity, Spawn);
	AActor* C0 = World->SpawnActor<AActor>(Child.Generated(), FTransform::Identity, Spawn);
	if (!TestNotNull(TEXT("placed parent actor"), P0) || !TestNotNull(TEXT("placed child actor"), C0)
		|| !TestNotNull(TEXT("placed child actor has its Mesh"), MeshOf(C0)))
	{
		return false;
	}
	SetPropagationDouble(P0, TEXT("Health"), 55.0);
	C0->InitialLifeSpan = 7.f;
	MeshOf(C0)->AnimClass = UAnimInstance::StaticClass();
	TestEqual(TEXT("child actor starts with the inherited Health"), PropagationDouble(C0, TEXT("Health")), 100.0);

	// The parent edit: a new variable with a default, a changed variable default, a changed class
	// default and a changed component property.
	const FString ParentEdit = Parent.Document(TEXT("Update"), ParentBody(TEXT("150.0"),
		TEXT(",{\"Name\":\"Armor\",\"Type\":\"float\",\"DefaultValue\":45.0}],"
		"\"ClassDefaults\":{\"InitialLifeSpan\":3.0},"
		"\"Components\":[{\"Name\":\"Mesh\",\"Class\":\"/Script/Engine.SkeletalMeshComponent\","
		"\"Properties\":{\"AnimClass\":\"/Script/Engine.AnimSingleNodeInstance\"}}]")));
	const FAssetDocumentResult Edited = Fixture.Apply(Parent, ParentEdit, TEXT("edit parent"));
	if (!TestTrue(TEXT("parent edit applies"), Edited.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the result says which dependents were updated"), Edited.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Graph.DefaultsPropagated"); }));

	// The child Blueprint's class defaults and Mesh override.
	const UObject* ChildDefaults = Child.Defaults();
	TestEqual(TEXT("child CDO: new variable default"), PropagationDouble(ChildDefaults, TEXT("Armor")), 45.0);
	TestEqual(TEXT("child CDO: changed inherited variable default"), PropagationDouble(ChildDefaults, TEXT("Health")), 150.0);
	TestEqual(TEXT("child CDO: changed inherited class default"), ChildDefaults ? Cast<AActor>(ChildDefaults)->InitialLifeSpan : -1.f, 3.f);
	TestEqual(TEXT("child CDO: its own Speed stays"), PropagationDouble(ChildDefaults, TEXT("Speed")), 20.0);
	const USkeletalMeshComponent* ChildMesh = InheritedTemplate<USkeletalMeshComponent>(Child.Get(), TEXT("Mesh"));
	TestTrue(TEXT("child Mesh override: changed inherited AnimClass"), AnimClassOf(ChildMesh) == UAnimSingleNodeInstance::StaticClass());
	TestTrue(TEXT("child Mesh override: its own bHiddenInGame stays"), ChildMesh != nullptr && ChildMesh->bHiddenInGame);
	TestFalse(TEXT("the child Blueprint stays saved"), Child.Get() == nullptr || Child.Get()->GetOutermost()->IsDirty());
	const TSharedPtr<FJsonObject> ChildBody = Fixture.ExtractBody(Child, TEXT("after parent edit"));
	const TSharedPtr<FJsonObject>* ClassDefaults = nullptr;
	TestEqual(TEXT("child extract: ClassDefaults are its own"),
		ChildBody.IsValid() && ChildBody->TryGetObjectField(TEXT("ClassDefaults"), ClassDefaults) ? Json(*ClassDefaults) : FString(),
		FString(TEXT("{\"Speed\":20.0}")));
	TestEqual(TEXT("child extract: Mesh override is its own"), ExtractedComponentProperties(ChildBody, TEXT("Mesh")),
		FString(TEXT("{\"bHiddenInGame\":true}")));

	// The placed actors (replaced by the compile's reinstancing: looked up again).
	P0 = PlacedActorOf(World, Parent.Generated());
	C0 = PlacedActorOf(World, Child.Generated());
	if (!TestNotNull(TEXT("placed parent actor after the edit"), P0) || !TestNotNull(TEXT("placed child actor after the edit"), C0))
	{
		return false;
	}
	TestEqual(TEXT("P0: new variable default"), PropagationDouble(P0, TEXT("Armor")), 45.0);
	TestEqual(TEXT("P0: its Health override stays"), PropagationDouble(P0, TEXT("Health")), 55.0);
	TestEqual(TEXT("P0: changed class default"), P0->InitialLifeSpan, 3.f);
	TestEqual(TEXT("P0: Speed unchanged"), PropagationDouble(P0, TEXT("Speed")), 10.0);
	TestTrue(TEXT("P0 Mesh: changed AnimClass"), AnimClassOf(MeshOf(P0)) == UAnimSingleNodeInstance::StaticClass());
	TestEqual(TEXT("C0: new variable default"), PropagationDouble(C0, TEXT("Armor")), 45.0);
	TestEqual(TEXT("C0: changed inherited variable default"), PropagationDouble(C0, TEXT("Health")), 150.0);
	TestEqual(TEXT("C0: its InitialLifeSpan override stays"), C0->InitialLifeSpan, 7.f);
	TestEqual(TEXT("C0: the child's Speed"), PropagationDouble(C0, TEXT("Speed")), 20.0);
	TestTrue(TEXT("C0 Mesh: its AnimClass override stays"), AnimClassOf(MeshOf(C0)) == UAnimInstance::StaticClass());
	TestTrue(TEXT("C0 Mesh: the child's bHiddenInGame"), MeshOf(C0) != nullptr && MeshOf(C0)->bHiddenInGame);

	// The same document again changes nothing.
	const FAssetDocumentResult Repeat = Fixture.Apply(Parent, ParentEdit, TEXT("repeat parent edit"));
	TestTrue(TEXT("repeated parent edit is a no-op"), Repeat.IsSuccess() && !Repeat.bSavedAsset);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintInheritedNullOverrideTest,
	"UEAssetDoc.Blueprint.DefaultPropagation.InheritedNullOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintInheritedNullOverrideTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDefaultPropagationTests;
	const FPropagationFixture Fixture(*this, {TEXT("BP_P66Base"), TEXT("BP_P66Mid"), TEXT("BP_P66Leaf")});
	const FPropagationAsset& Base = Fixture[0];
	const FPropagationAsset& Mid = Fixture[1];
	const FPropagationAsset& Leaf = Fixture[2];
	const FString LeafDocument = Leaf.Document(TEXT("CreateOrUpdate"), FString::Printf(TEXT("\"ParentClass\":\"%s\","
		"\"Components\":[{\"Name\":\"Mesh\",\"Scope\":\"Inherited\",\"Properties\":{\"StaticMesh\":null}}]"), *Mid.ClassPath));
	if (!Fixture.ApplyOk(Base, Base.Document(TEXT("Create"), TEXT("\"ParentClass\":\"/Script/Engine.Actor\","
			"\"Components\":[{\"Name\":\"Mesh\",\"Class\":\"/Script/Engine.StaticMeshComponent\"}]")), TEXT("create base"))
		|| !Fixture.ApplyOk(Mid, Mid.Document(TEXT("Create"), FString::Printf(TEXT("\"ParentClass\":\"%s\","
			"\"Components\":[{\"Name\":\"Mesh\",\"Scope\":\"Inherited\",\"Properties\":{\"StaticMesh\":\"%s\"}}]"), *Base.ClassPath, Cube)),
			TEXT("create mid"))
		|| !Fixture.ApplyOk(Leaf, LeafDocument, TEXT("create leaf")))
	{
		return false;
	}
	const UStaticMeshComponent* MidMesh = InheritedTemplate<UStaticMeshComponent>(Mid.Get(), TEXT("Mesh"));
	TestTrue(TEXT("mid overrides the mesh"), MidMesh != nullptr && MidMesh->GetStaticMesh() != nullptr
		&& MidMesh->GetStaticMesh()->GetPathName() == Cube);
	const UStaticMeshComponent* LeafMesh = InheritedTemplate<UStaticMeshComponent>(Leaf.Get(), TEXT("Mesh"));
	TestTrue(TEXT("leaf override holds null"), LeafMesh != nullptr && LeafMesh->GetStaticMesh() == nullptr);
	TestEqual(TEXT("extract shows the null override"), ExtractedComponentProperties(Fixture.ExtractBody(Leaf, TEXT("after create")), TEXT("Mesh")),
		FString(TEXT("{\"StaticMesh\":null}")));
	const FAssetDocumentResult Repeat = Fixture.Apply(Leaf, LeafDocument, TEXT("repeat leaf"));
	TestTrue(TEXT("re-applying the leaf is a no-op"), Repeat.IsSuccess() && !Repeat.bSavedAsset);

	// Saved and loaded back from disk, the override is still null.
	Fixture.Unload();
	TestNotNull(TEXT("leaf loads back"), LoadObject<UBlueprint>(nullptr, *Leaf.ObjectPath));
	LeafMesh = InheritedTemplate<UStaticMeshComponent>(Leaf.Get(), TEXT("Mesh"));
	TestTrue(TEXT("reloaded leaf override holds null"), LeafMesh != nullptr && LeafMesh->GetStaticMesh() == nullptr);
	TestEqual(TEXT("reloaded extract shows the null override"), ExtractedComponentProperties(Fixture.ExtractBody(Leaf, TEXT("after reload")), TEXT("Mesh")),
		FString(TEXT("{\"StaticMesh\":null}")));

	// Removing the override (the component unlisted) gives the mid value back.
	if (Fixture.ApplyOk(Leaf, Leaf.Document(TEXT("Update"), FString::Printf(TEXT("\"ParentClass\":\"%s\",\"Components\":[]"), *Mid.ClassPath)),
		TEXT("reset leaf")))
	{
		LeafMesh = InheritedTemplate<UStaticMeshComponent>(Leaf.Get(), TEXT("Mesh"));
		TestTrue(TEXT("reset leaf holds the mid mesh"), LeafMesh == nullptr
			|| (LeafMesh->GetStaticMesh() != nullptr && LeafMesh->GetStaticMesh()->GetPathName() == Cube));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintEmptyEventGraphTest,
	"UEAssetDoc.Blueprint.EmptyEventGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintEmptyEventGraphTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDefaultPropagationTests;
	const FPropagationFixture Fixture(*this, {TEXT("BP_P66Events")});
	const FPropagationAsset& Asset = Fixture[0];
	// BeginPlay sets Count and hides Spear: the nodes use the variable and the component.
	const FString Created = Asset.Document(TEXT("Create"), TEXT("\"ParentClass\":\"/Script/Engine.Actor\","
		"\"Variables\":[{\"Name\":\"Count\",\"Type\":\"int\",\"DefaultValue\":1}],"
		"\"Components\":[{\"Name\":\"Spear\",\"Class\":\"/Script/Engine.StaticMeshComponent\"}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
		"{\"Id\":\"SetCount\",\"Action\":\"K2:Set:Count\",\"PinOverrides\":[{\"Pin\":\"Count\",\"DefaultValue\":\"2\"}]},"
		"{\"Id\":\"GetSpear\",\"Action\":\"K2:Get:Spear\"},"
		"{\"Id\":\"Hide\",\"Action\":\"K2:CallFunction:/Script/Engine.SceneComponent:SetVisibility\"}],"
		"\"Links\":[{\"From\":\"Begin:then\",\"To\":\"SetCount:execute\"},{\"From\":\"SetCount:then\",\"To\":\"Hide:execute\"},"
		"{\"From\":\"GetSpear:Spear\",\"To\":\"Hide:self\"}]}]"));
	if (!Fixture.ApplyOk(Asset, Created, TEXT("create")))
	{
		return false;
	}
	// Variables, functions and the event graph emptied in one apply.
	const FString Emptied = Asset.Document(TEXT("Update"), TEXT("\"ParentClass\":\"/Script/Engine.Actor\","
		"\"Variables\":[],\"Components\":[],\"FunctionGraphs\":[],\"UbergraphPages\":[]"));
	const FAssetDocumentResult Result = Fixture.Apply(Asset, Emptied, TEXT("empty"));
	if (!TestTrue(TEXT("emptying applies (no compile error from nodes using removed members)"), Result.IsSuccess()))
	{
		return false;
	}
	UBlueprint* Blueprint = Asset.Get();
	TestTrue(TEXT("compiles"), Blueprint != nullptr && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings));
	TestEqual(TEXT("the EventGraph is kept"), Blueprint ? Blueprint->UbergraphPages.Num() : 0, 1);
	TArray<FString> Left;
	for (const UEdGraph* Graph : Blueprint ? Blueprint->UbergraphPages : TArray<TObjectPtr<UEdGraph>>())
	{
		for (const UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			// The disabled placeholder events of a new Blueprint (ghost nodes) are not user nodes.
			const bool bGhost = Node != nullptr && Node->IsA<UK2Node_Event>() && !Node->IsNodeEnabled()
				&& !Node->Pins.ContainsByPredicate([](const UEdGraphPin* Pin) { return Pin && !Pin->LinkedTo.IsEmpty(); });
			if (Node != nullptr && !bGhost)
			{
				Left.Add(Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}
	}
	TestEqual(*FString::Printf(TEXT("the EventGraph holds no nodes (%s)"), *FString::Join(Left, TEXT(", "))), Left.Num(), 0);
	TestEqual(TEXT("variables removed"), Blueprint ? Blueprint->NewVariables.Num() : -1, 0);
	const FAssetDocumentResult Repeat = Fixture.Apply(Asset, Emptied, TEXT("repeat empty"));
	TestTrue(TEXT("re-applying is a no-op"), Repeat.IsSuccess() && !Repeat.bSavedAsset);
	return !HasAnyErrors();
}
#endif

