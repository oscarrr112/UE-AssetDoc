// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Functions declared on an interface, called on a class that implements it
// (IEnhancedInputSubsystemInterface's AddMappingContext / GetUserSettings on the Enhanced Input local player
// subsystem), on the production route.
//   InterfaceCallKey  6  the key naming the implementing class (as an AI writes it, and as the editor's menu off a
//                        subsystem pin offers it) validates, applies, re-applies with no change and reads back under
//                        the declaring interface's key; Validate checks its pins as Apply would (a misspelt pin fails
//                        Validate too); list_types accepts that key and names the declaring key.
//   ListTypesTarget   7  list_types blueprint_node target=<class>: the functions callable on an object of that class,
//                        its interfaces' functions included, as dragging off a pin of that class lists them; without
//                        target the search lists them too, naming their class.
//   ListTypesTitle    7  list_types blueprint_node name=<menu title> without target: functions of other classes are
//                        found too (the menu with Context Sensitive off), the exact title first, naming their class.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintInterfaceCallTests
{
const TCHAR* const SubsystemClass = TEXT("/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem");
const TCHAR* const SubclassAddKey = TEXT("K2:CallFunction:/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem:AddMappingContext");
const TCHAR* const InterfaceAddKey = TEXT("K2:CallFunction:/Script/EnhancedInput.EnhancedInputSubsystemInterface:AddMappingContext");
const TCHAR* const InterfaceSettingsKey = TEXT("K2:CallFunction:/Script/EnhancedInput.EnhancedInputSubsystemInterface:GetUserSettings");

struct FBlueprintTarget
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;
	FString Folder;

	explicit FBlueprintTarget(const TCHAR* InName)
		: Name(InName)
		, Folder(FString(TEXT("AssetDocumentP95/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12))
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
	}

	~FBlueprintTarget()
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			UPackageTools::UnloadPackages({Package});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*PackageFile, false, true);
		IFileManager::Get().Delete(*SidecarPath, false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, true);
	}

	UBlueprint* Get() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }

	// BeginPlay -> AddMappingContext on the player's input subsystem, the call keyed by CallKey.
	FString Document(const TCHAR* Action, const TCHAR* CallKey, const TCHAR* SelfPin = TEXT("self")) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",")
			TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Pawn\",\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[")
			TEXT("{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},")
			TEXT("{\"Id\":\"PC\",\"Action\":\"K2:CallFunction:/Script/Engine.GameplayStatics:GetPlayerController\"},")
			TEXT("{\"Id\":\"Input\",\"Action\":\"K2:GetSubsystemFromPC:%s\"},")
			TEXT("{\"Id\":\"Add\",\"Action\":\"%s\"}],")
			TEXT("\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Add:execute\"},{\"From\":\"PC:ReturnValue\",\"To\":\"Input:PlayerController\"},")
			TEXT("{\"From\":\"Input:ReturnValue\",\"To\":\"Add:%s\"}]}]}}"),
			*PackageName, Action, SubsystemClass, CallKey, SelfPin);
	}
};

void Report(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path,
			*Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FBlueprintTarget& Target, const FString& Json, const FString& Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Stage, Result);
	return Result;
}

FAssetDocumentResult Validate(FAutomationTestBase& Test, const FString& Json, const FString& Stage)
{
	FAssetDocumentValidateRequest Request;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Request.Document);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	Report(Test, Stage, Result);
	return Result;
}

TArray<FString> ListPaths(FAutomationTestBase& Test, const TMap<FString, FString>& Query, const FString& Stage, FString* OutMessage = nullptr)
{
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ListTypes(Query);
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Types"), Rows))
	{
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			Paths.Add(Row->AsObject()->GetStringField(TEXT("Path")));
		}
	}
	else
	{
		Report(Test, Stage, Result);
	}
	if (OutMessage != nullptr)
	{
		*OutMessage = Result.Message;
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			*OutMessage += TEXT(" ") + Diagnostic.Message;
		}
	}
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *FString::Join(Paths, TEXT(", "))));
	return Paths;
}

// The rows list_types returns (empty on failure).
TArray<TSharedPtr<FJsonObject>> ListRows(FAutomationTestBase& Test, const TMap<FString, FString>& Query, const FString& Stage)
{
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ListTypes(Query);
	TArray<TSharedPtr<FJsonObject>> Out;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Result.IsSuccess() || !Result.Payload.IsValid() || !Result.Payload->TryGetArrayField(TEXT("Types"), Rows))
	{
		Report(Test, Stage, Result);
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& Row : *Rows)
	{
		Out.Add(Row->AsObject());
		FString Target;
		Out.Last()->TryGetStringField(TEXT("Target"), Target);
		FString Title;
		Out.Last()->TryGetStringField(TEXT("Title"), Title);
		Test.AddInfo(FString::Printf(TEXT("%s: %s '%s' (Target %s)"), *Stage, *Out.Last()->GetStringField(TEXT("Path")), *Title, *Target));
	}
	return Out;
}

// The Blueprint's AddMappingContext call node (its function and the interface declaring it).
const UK2Node_CallFunction* FindAddCall(const UBlueprint* Blueprint)
{
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
			if (Call != nullptr && Call->FunctionReference.GetMemberName() == TEXT("AddMappingContext"))
			{
				return Call;
			}
		}
	}
	return nullptr;
}
} // namespace AssetDocumentBlueprintInterfaceCallTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintInterfaceCallKeyTest,
	"UEAssetDoc.Graph.Blueprint.InterfaceCallKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintInterfaceCallKeyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentBlueprintInterfaceCallTests;
	if (!TestNotNull(TEXT("Enhanced Input is loaded"), FSoftClassPath(SubsystemClass).ResolveClass()))
	{
		return false;
	}
	const FBlueprintTarget Target(TEXT("BP_InterfaceCall"));
	// Validate reads the pins of the action Apply places: a pin the call does not have fails both.
	const FAssetDocumentResult Misspelt = Validate(*this, Target.Document(TEXT("Create"), SubclassAddKey, TEXT("Subsystem")), TEXT("wrong pin"));
	TestFalse(TEXT("Validate refuses a pin the call does not have"), Misspelt.IsSuccess());

	const FString Json = Target.Document(TEXT("Create"), SubclassAddKey);
	TestTrue(TEXT("the implementing class's key validates"), Validate(*this, Json, TEXT("validate")).IsSuccess());
	if (!TestTrue(TEXT("the implementing class's key applies"), Apply(*this, Target, Json, TEXT("apply")).IsSuccess()))
	{
		return false;
	}
	const UK2Node_CallFunction* Call = FindAddCall(Target.Get());
	if (TestNotNull(TEXT("the call is placed"), Call))
	{
		TestEqual(TEXT("it calls the interface's function"), GetPathNameSafe(Call->GetTargetFunction() ? Call->GetTargetFunction()->GetOwnerClass() : nullptr),
			FString(TEXT("/Script/EnhancedInput.EnhancedInputSubsystemInterface")));
	}
	const FString Update = Target.Document(TEXT("Update"), SubclassAddKey);
	TestTrue(TEXT("the same document validates against the asset"), Validate(*this, Update, TEXT("validate again")).IsSuccess());
	const FAssetDocumentResult Again = Apply(*this, Target, Update, TEXT("apply again"));
	TestTrue(TEXT("re-apply succeeds"), Again.IsSuccess());
	TestFalse(TEXT("re-apply changes nothing"), Again.bSavedAsset);
	const FAssetDocumentResult Declared = Apply(*this, Target, Target.Document(TEXT("Update"), InterfaceAddKey), TEXT("declared key"));
	TestTrue(TEXT("the declaring key is the same node"), Declared.IsSuccess() && !Declared.bSavedAsset);

	// list_types accepts the implementing class's key and names the key the node reads back under.
	const TArray<FString> Listed = ListPaths(*this, {{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("base"), TEXT("/Script/Engine.Pawn")},
		{TEXT("name"), SubclassAddKey}}, TEXT("list_types key"));
	TestTrue(TEXT("list_types resolves the implementing class's key to the interface's"), Listed.Contains(InterfaceAddKey));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintListTypesTargetTest,
	"UEAssetDoc.Graph.Blueprint.ListTypesTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintListTypesTargetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentBlueprintInterfaceCallTests;
	// Without a target, another class's member function is listed as the menu with Context Sensitive off lists it,
	// naming the class whose object its self pin takes.
	const TArray<TSharedPtr<FJsonObject>> Plain = ListRows(*this, {{TEXT("kind"), TEXT("blueprint_node")},
		{TEXT("base"), TEXT("/Script/Engine.Character")}, {TEXT("name"), TEXT("AddMappingContext")}}, TEXT("no target"));
	TestTrue(TEXT("no target: listed with its class as the target"), Plain.ContainsByPredicate([](const TSharedPtr<FJsonObject>& Row)
	{
		FString Target;
		return Row->GetStringField(TEXT("Path")) == InterfaceAddKey && Row->TryGetStringField(TEXT("Target"), Target)
			&& Target == TEXT("/Script/EnhancedInput.EnhancedInputSubsystemInterface");
	}));
	const TArray<FString> Targeted = ListPaths(*this, {{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("base"), TEXT("/Script/Engine.Character")},
		{TEXT("name"), TEXT("AddMappingContext")}, {TEXT("target"), SubsystemClass}}, TEXT("target"));
	TestTrue(TEXT("target subsystem: the interface's AddMappingContext"), Targeted.Contains(InterfaceAddKey));
	const TArray<FString> Settings = ListPaths(*this, {{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("base"), TEXT("/Script/Engine.Character")},
		{TEXT("name"), TEXT("GetUserSettings")}, {TEXT("target"), SubsystemClass}}, TEXT("target settings"));
	TestTrue(TEXT("target subsystem: the interface's GetUserSettings"), Settings.Contains(InterfaceSettingsKey));
	FString Message;
	const TArray<FString> Unknown = ListPaths(*this, {{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("name"), TEXT("AddMappingContext")},
		{TEXT("target"), TEXT("/Script/EnhancedInput.NoSuchSubsystem")}}, TEXT("unknown target"), &Message);
	TestTrue(*FString::Printf(TEXT("an unknown target is refused naming it (%s)"), *Message), Unknown.IsEmpty() && Message.Contains(TEXT("NoSuchSubsystem")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintListTypesTitleTest,
	"UEAssetDoc.Graph.Blueprint.ListTypesTitle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintListTypesTitleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentBlueprintInterfaceCallTests;
	const TCHAR* const Blackboard = TEXT("/Script/AIModule.BlackboardComponent");
	const FString SetObjectKey = FString(TEXT("K2:CallFunction:")) + Blackboard + TEXT(":SetValueAsObject");
	// The menu title of a function of another class (the Blackboard component's), searched from an AI Controller
	// Blueprint without a target: it comes first, naming the class whose object its self pin takes.
	const TArray<TSharedPtr<FJsonObject>> Rows = ListRows(*this, {{TEXT("kind"), TEXT("blueprint_node")},
		{TEXT("base"), TEXT("/Script/AIModule.AIController")}, {TEXT("name"), TEXT("Set Value as Object")}}, TEXT("title"));
	if (TestTrue(TEXT("title: rows"), !Rows.IsEmpty()))
	{
		TestEqual(TEXT("title: the exact title first"), Rows[0]->GetStringField(TEXT("Path")), SetObjectKey);
		FString Target;
		Rows[0]->TryGetStringField(TEXT("Target"), Target);
		TestEqual(TEXT("title: its class is the target"), Target, FString(Blackboard));
		TestTrue(TEXT("title: its pins"), Rows[0]->HasTypedField<EJson::Array>(TEXT("Pins")));
	}
	// The same words with the target: the target's functions only, no Target field (it is the target).
	const TArray<TSharedPtr<FJsonObject>> Targeted = ListRows(*this, {{TEXT("kind"), TEXT("blueprint_node")},
		{TEXT("base"), TEXT("/Script/AIModule.AIController")}, {TEXT("name"), TEXT("Set Value as Object")}, {TEXT("target"), Blackboard}},
		TEXT("target"));
	TestTrue(TEXT("target: listed"), Targeted.ContainsByPredicate([&SetObjectKey](const TSharedPtr<FJsonObject>& Row)
	{
		return Row->GetStringField(TEXT("Path")) == SetObjectKey && !Row->HasField(TEXT("Target"));
	}));
	// The context's own functions keep no Target (an AI Controller's MoveToActor).
	const TArray<TSharedPtr<FJsonObject>> Own = ListRows(*this, {{TEXT("kind"), TEXT("blueprint_node")},
		{TEXT("base"), TEXT("/Script/AIModule.AIController")}, {TEXT("name"), TEXT("Move to Actor")}}, TEXT("own"));
	if (TestTrue(TEXT("own: rows"), !Own.IsEmpty()))
	{
		TestEqual(TEXT("own: the context's function first"), Own[0]->GetStringField(TEXT("Path")),
			FString(TEXT("K2:CallFunction:/Script/AIModule.AIController:MoveToActor")));
		TestFalse(TEXT("own: no Target"), Own[0]->HasField(TEXT("Target")));
	}
	return true;
}
#endif
