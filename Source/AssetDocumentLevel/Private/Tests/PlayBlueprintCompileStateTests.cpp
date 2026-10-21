// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// A Blueprint stuck Dirty around PIE, on the production Blueprint route.
//   (a) A Blueprint apply ends with the Blueprint compiled (compile status UpToDate) and its package saved, also for a
//       remap graph (a custom event with a struct input, GetUserSettings -> MakeStruct -> MapPlayerKey ...) and
//       after the Input Actions it binds were changed.
//   (b) Starting PIE compiles what needs compiling; that compile is not an unsaved user edit: the next apply of the
//       Blueprint is accepted when the package had no unsaved changes before PIE.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "LevelTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Play/PlaySession.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace PlayBlueprintCompileStateTests
{
using namespace AssetDocumentLevelTests;
using UE::AssetDocumentLevel::Private::FLevelToolResult;
namespace Play = UE::AssetDocumentLevel::Private::Play;

struct FAsset
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	FAsset(const FString& Folder, const TCHAR* InName)
		: Name(InName)
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }
	UBlueprint* Blueprint() const { return Cast<UBlueprint>(Get()); }
	bool IsDirty() const { const UObject* Asset = Get(); return Asset != nullptr && Asset->GetOutermost()->IsDirty(); }

	void Delete() const
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			UPackageTools::UnloadPackages({Package});
		}
		IFileManager::Get().Delete(*PackageFile, false, true);
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

struct FState
{
	FString Folder;
	TArray<FAsset> Assets;
	bool bAppliedAfterPlay = false;

	const FAsset& Jump() const { return Assets[0]; }
	const FAsset& Context() const { return Assets[1]; }
	const FAsset& Hero() const { return Assets[2]; }

	~FState()
	{
		for (int32 Index = Assets.Num() - 1; Index >= 0; --Index)
		{
			Assets[Index].Delete();
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TArray<FString> Files;
		for (const FAsset& Asset : Assets)
		{
			Files.Add(Asset.PackageFile);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, true);
	}
};

FString Codes(const FAssetDocumentResult& Result)
{
	TArray<FString> Out;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Out.Add(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	return Result.Message + TEXT(" ") + FString::Join(Out, TEXT(" | "));
}

FString CompileStatus(const FAssetDocumentResult& Result)
{
	FString Status;
	return Result.Compile.IsValid() && Result.Compile->TryGetStringField(TEXT("Status"), Status) ? Status : FString(TEXT("(none)"));
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FAsset& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Test.AddInfo(FString::Printf(TEXT("%s: compile %s, package %s; %s"), Stage, *CompileStatus(Result),
		Target.IsDirty() ? TEXT("dirty") : TEXT("clean"), *Codes(Result)));
	return Result;
}

FAssetDocumentResult Validate(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentValidateRequest Request;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Request.Document);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Codes(Result)));
	return Result;
}

FString Generic(const FAsset& Target, const TCHAR* Class, const FString& Properties)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
		TEXT("\"Action\":\"CreateOrUpdate\",\"Properties\":%s}"), *Target.PackageName, Class, *Properties);
}

// The character: BeginPlay adds the mapping context (Options notify the user settings), IA_Jump jumps; with
// bRemap, the custom event RebindJump(NewKey) remaps the player's Jump key and saves the user settings.
FString HeroDocument(const FState& State, bool bRemap, const TCHAR* PrintText)
{
	FString Nodes = FString::Printf(
		TEXT("{\"Id\":\"BeginPlay\",\"Action\":\"K2:BeginPlay\"},")
		TEXT("{\"Id\":\"GetController\",\"Action\":\"K2:CallFunction:/Script/Engine.Pawn:GetController\"},")
		TEXT("{\"Id\":\"Cast\",\"Action\":\"K2:DynamicCast:/Script/Engine.PlayerController\"},")
		TEXT("{\"Id\":\"Input\",\"Action\":\"K2:GetSubsystemFromPC:/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem\"},")
		TEXT("{\"Id\":\"Add\",\"Action\":\"K2:CallFunction:/Script/EnhancedInput.EnhancedInputSubsystemInterface:AddMappingContext\",")
		TEXT("\"PinOverrides\":[{\"Pin\":\"MappingContext\",\"DefaultObject\":\"%s\"},{\"Pin\":\"Options\",")
		TEXT("\"DefaultValue\":\"(bIgnoreAllPressedKeysUntilRelease=True,bForceImmediately=False,bNotifyUserSettings=True)\"}]},")
		TEXT("{\"Id\":\"IA_Jump\",\"Action\":\"K2:EnhancedInputAction:%s\"},")
		TEXT("{\"Id\":\"Jump\",\"Action\":\"K2:CallFunction:/Script/Engine.Character:Jump\"},")
		TEXT("{\"Id\":\"Print\",\"Action\":\"K2:PrintString\",\"PinOverrides\":[{\"Pin\":\"InString\",\"DefaultValue\":\"%s\"}]}"),
		*State.Context().ObjectPath, *State.Jump().ObjectPath, PrintText);
	FString Links = TEXT("{\"From\":\"BeginPlay:then\",\"To\":\"Cast:execute\"},{\"From\":\"GetController:ReturnValue\",\"To\":\"Cast:Object\"},")
		TEXT("{\"From\":\"Cast:then\",\"To\":\"Add:execute\"},{\"From\":\"Cast:AsPlayer Controller\",\"To\":\"Input:PlayerController\"},")
		TEXT("{\"From\":\"Input:ReturnValue\",\"To\":\"Add:self\"},{\"From\":\"IA_Jump:Started\",\"To\":\"Jump:execute\"},")
		TEXT("{\"From\":\"Jump:then\",\"To\":\"Print:execute\"}");
	if (bRemap)
	{
		Nodes += TEXT(",{\"Id\":\"RebindJump\",\"Action\":\"K2:CustomEvent\",\"Name\":\"RebindJump\",")
			TEXT("\"Inputs\":[{\"Name\":\"NewKey\",\"Type\":\"struct:/Script/InputCore.Key\"}]},")
			TEXT("{\"Id\":\"GetController_2\",\"Action\":\"K2:CallFunction:/Script/Engine.Pawn:GetController\"},")
			TEXT("{\"Id\":\"Cast_2\",\"Action\":\"K2:DynamicCast:/Script/Engine.PlayerController\"},")
			TEXT("{\"Id\":\"Input_2\",\"Action\":\"K2:GetSubsystemFromPC:/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem\"},")
			TEXT("{\"Id\":\"GetUserSettings\",\"Action\":\"K2:CallFunction:/Script/EnhancedInput.EnhancedInputSubsystemInterface:GetUserSettings\"},")
			TEXT("{\"Id\":\"Args\",\"Action\":\"K2:MakeStruct:/Script/EnhancedInput.MapPlayerKeyArgs\",")
			TEXT("\"PinOverrides\":[{\"Pin\":\"MappingName\",\"DefaultValue\":\"Jump\"},{\"Pin\":\"Slot\",\"DefaultValue\":\"First\"}]},")
			TEXT("{\"Id\":\"MapPlayerKey\",\"Action\":\"K2:CallFunction:/Script/EnhancedInput.EnhancedInputUserSettings:MapPlayerKey\"},")
			TEXT("{\"Id\":\"ApplySettings\",\"Action\":\"K2:CallFunction:/Script/EnhancedInput.EnhancedInputUserSettings:ApplySettings\"},")
			TEXT("{\"Id\":\"SaveSettings\",\"Action\":\"K2:CallFunction:/Script/EnhancedInput.EnhancedInputUserSettings:SaveSettings\"}");
		Links += TEXT(",{\"From\":\"RebindJump:then\",\"To\":\"Cast_2:execute\"},{\"From\":\"RebindJump:NewKey\",\"To\":\"Args:NewKey\"},")
			TEXT("{\"From\":\"GetController_2:ReturnValue\",\"To\":\"Cast_2:Object\"},{\"From\":\"Cast_2:then\",\"To\":\"MapPlayerKey:execute\"},")
			TEXT("{\"From\":\"Cast_2:AsPlayer Controller\",\"To\":\"Input_2:PlayerController\"},")
			TEXT("{\"From\":\"Input_2:ReturnValue\",\"To\":\"GetUserSettings:self\"},")
			TEXT("{\"From\":\"GetUserSettings:ReturnValue\",\"To\":\"MapPlayerKey:self\"},")
			TEXT("{\"From\":\"GetUserSettings:ReturnValue\",\"To\":\"ApplySettings:self\"},")
			TEXT("{\"From\":\"GetUserSettings:ReturnValue\",\"To\":\"SaveSettings:self\"},")
			TEXT("{\"From\":\"Args:MapPlayerKeyArgs\",\"To\":\"MapPlayerKey:InArgs\"},")
			TEXT("{\"From\":\"MapPlayerKey:then\",\"To\":\"ApplySettings:execute\"},{\"From\":\"ApplySettings:then\",\"To\":\"SaveSettings:execute\"}");
	}
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"CreateOrUpdate\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Character\",\"Variables\":[{\"Name\":\"WalkSpeed\",\"Type\":\"float\",\"DefaultValue\":500}],")
		TEXT("\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[%s],\"Links\":[%s]}]}}"),
		*State.Hero().PackageName, *Nodes, *Links);
}

void ReportBlueprint(FAutomationTestBase& Test, const FAsset& Hero, const TCHAR* Stage)
{
	const UBlueprint* Blueprint = Hero.Blueprint();
	Test.AddInfo(FString::Printf(TEXT("%s: Blueprint status %d, package %s"), Stage, Blueprint != nullptr ? static_cast<int32>(Blueprint->Status) : -1,
		Hero.IsDirty() ? TEXT("dirty") : TEXT("clean")));
}

class FPlayCall final : public IAutomationLatentCommand
{
public:
	FPlayCall(FString InBody, TFunction<void(const FLevelToolResult&)> InOnDone) : Body(MoveTemp(InBody)), OnDone(MoveTemp(InOnDone)) {}

	virtual bool Update() override
	{
		const FLevelToolResult Result = Play::RunPlayFromBody(bSent ? FString::Printf(TEXT("{\"action\": \"poll\", \"id\": %d}"), Id) : Body);
		bSent = true;
		const TSharedPtr<FJsonObject>* Pending = nullptr;
		if (Result.bSuccess && Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("pending"), Pending) && Pending != nullptr)
		{
			Id = static_cast<int32>((*Pending)->GetNumberField(TEXT("id")));
			return false;
		}
		OnDone(Result);
		return true;
	}

private:
	FString Body;
	TFunction<void(const FLevelToolResult&)> OnDone;
	bool bSent = false;
	int32 Id = 0;
};

// Some editor frames (the editor's tickables run between tool calls).
class FFrames final : public IAutomationLatentCommand
{
public:
	FFrames(int32 InFrames, TFunction<void()> InThen) : Frames(InFrames), Then(MoveTemp(InThen)) {}

	virtual bool Update() override
	{
		if (--Frames > 0)
		{
			return false;
		}
		Then();
		return true;
	}

private:
	int32 Frames = 0;
	TFunction<void()> Then;
};

FString PlayCodes(const FLevelToolResult& Result)
{
	TArray<FString> Out;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Out.Add(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	return FString::Join(Out, TEXT(" | "));
}
} // namespace PlayBlueprintCompileStateTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayBlueprintCompileStateTest,
	"UEAssetDoc.Level.Play.BlueprintCompileState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayBlueprintCompileStateTest::RunTest(const FString& Parameters)
{
	using namespace PlayBlueprintCompileStateTests;
	if (!TestNull(TEXT("no play session is running"), GEditor->PlayWorld.Get()))
	{
		return false;
	}
	TSharedRef<FState> State = MakeShared<FState>();
	State->Folder = FString(TEXT("AssetDocumentCompile/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10);
	State->Assets = {FAsset(State->Folder, TEXT("IA_Jump")), FAsset(State->Folder, TEXT("IMC_Hero")), FAsset(State->Folder, TEXT("BP_InputHero"))};
	const TCHAR* const ActionClass = TEXT("/Script/EnhancedInput.InputAction");
	if (!TestTrue(TEXT("IA_Jump applies"), Apply(*this, State->Jump(), Generic(State->Jump(), ActionClass, TEXT("{\"ValueType\":\"Boolean\"}")),
			TEXT("IA_Jump")).IsSuccess())
		|| !TestTrue(TEXT("IMC_Hero applies"), Apply(*this, State->Context(), Generic(State->Context(), TEXT("/Script/EnhancedInput.InputMappingContext"),
			FString::Printf(TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[{\"Action\":\"%s\",\"Key\":\"SpaceBar\"}]}}"), *State->Jump().ObjectPath)),
			TEXT("IMC_Hero")).IsSuccess()))
	{
		return false;
	}
	const FAssetDocumentResult Created = Apply(*this, State->Hero(), HeroDocument(*State, false, TEXT("Jumped")), TEXT("hero"));
	if (!TestTrue(TEXT("the hero applies"), Created.IsSuccess()))
	{
		return false;
	}
	TestEqual(TEXT("(a) the hero apply compiles it"), CompileStatus(Created), FString(TEXT("UpToDate")));
	TestFalse(TEXT("(a) the hero apply saves it"), State->Hero().IsDirty());

	// Task 4c: the actions become player-mappable, then the remap graph is added.
	ADD_LATENT_AUTOMATION_COMMAND(FFrames(3, [this, State]()
	{
		const FAsset& Jump = State->Jump();
		TestTrue(TEXT("IA_Jump becomes player-mappable"), Apply(*this, Jump, FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",")
			TEXT("\"Target\":\"%s\",\"Class\":\"/Script/EnhancedInput.InputAction\",\"Action\":\"Update\",\"Properties\":{\"PlayerMappableKeySettings\":")
			TEXT("{\"Class\":\"/Script/EnhancedInput.PlayerMappableKeySettings\",\"Properties\":{\"Name\":\"Jump\",\"DisplayName\":\"Jump\"}}}}"),
			*Jump.PackageName), TEXT("IA_Jump mappable")).IsSuccess());
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFrames(3, [this, State]()
	{
		ReportBlueprint(*this, State->Hero(), TEXT("after the action change"));
		const FAssetDocumentResult Remap = Apply(*this, State->Hero(), HeroDocument(*State, true, TEXT("Jumped")), TEXT("hero remap"));
		TestTrue(TEXT("the remap graph applies"), Remap.IsSuccess());
		TestEqual(TEXT("(a) the remap apply compiles the hero"), CompileStatus(Remap), FString(TEXT("UpToDate")));
		TestFalse(TEXT("(a) the remap apply leaves the hero saved"), State->Hero().IsDirty());
		ReportBlueprint(*this, State->Hero(), TEXT("after the remap apply"));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFrames(3, [this, State]()
	{
		ReportBlueprint(*this, State->Hero(), TEXT("frames after the remap apply"));
		TestFalse(TEXT("(a) nothing dirties the hero after the apply"), State->Hero().IsDirty());
		// The hero plays: placed and possessed in a new map.
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
		const UBlueprint* Blueprint = State->Hero().Blueprint();
		UWorld* World = GetEditorWorld();
		APawn* Hero = World != nullptr && Blueprint != nullptr && Blueprint->GeneratedClass != nullptr
			? Cast<APawn>(SpawnActor(*World->PersistentLevel, Blueprint->GeneratedClass, FVector(0.0, 0.0, 300.0), TEXT("Hero"))) : nullptr;
		if (TestNotNull(TEXT("hero placed"), Hero))
		{
			Hero->AutoPossessPlayer = EAutoReceiveInput::Player0;
		}
		ReportBlueprint(*this, State->Hero(), TEXT("before play"));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"start\", \"paused\": true}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("start (%s)"), *PlayCodes(Result)), Result.bSuccess);
		ReportBlueprint(*this, State->Hero(), TEXT("playing"));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"wait\", \"seconds\": 0.2}"), [this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("wait (%s)"), *PlayCodes(Result)), Result.bSuccess);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"stop\"}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop (%s)"), *PlayCodes(Result)), Result.bSuccess);
		ReportBlueprint(*this, State->Hero(), TEXT("after play"));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFrames(3, [this, State]()
	{
		ReportBlueprint(*this, State->Hero(), TEXT("frames after play"));
		// (b) The next apply after PIE: the play session's compile is no unsaved user edit.
		const FString Next = HeroDocument(*State, true, TEXT("Jumped high"));
		TestTrue(TEXT("(b) the next document validates after play"), Validate(*this, Next, TEXT("validate after play")).IsSuccess());
		const FAssetDocumentResult Applied = Apply(*this, State->Hero(), Next, TEXT("apply after play"));
		TestTrue(TEXT("(b) the next apply after play is accepted"), Applied.IsSuccess());
		TestEqual(TEXT("(b) and compiles the hero"), CompileStatus(Applied), FString(TEXT("UpToDate")));
		TestFalse(TEXT("(b) and saves it"), State->Hero().IsDirty());
	}));
	// (b) A Blueprint whose compile status is stale with no unsaved changes (as after an apply): PIE compiles it, and
	// the apply after that is still accepted.
	ADD_LATENT_AUTOMATION_COMMAND(FFrames(2, [this, State]()
	{
		if (UBlueprint* Blueprint = State->Hero().Blueprint())
		{
			Blueprint->Status = BS_Dirty;
		}
		ReportBlueprint(*this, State->Hero(), TEXT("stale status"));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"start\", \"paused\": true}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("start again (%s)"), *PlayCodes(Result)), Result.bSuccess);
		ReportBlueprint(*this, State->Hero(), TEXT("playing after the stale status"));
		const UBlueprint* Blueprint = State->Hero().Blueprint();
		TestTrue(TEXT("play compiled the stale Blueprint"), Blueprint != nullptr && Blueprint->IsUpToDate());
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"stop\"}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop again (%s)"), *PlayCodes(Result)), Result.bSuccess);
		const FAssetDocumentResult Applied = Apply(*this, State->Hero(), HeroDocument(*State, true, TEXT("Jumped again")), TEXT("apply after the play compile"));
		TestTrue(TEXT("(b) the apply after the play compile is accepted"), Applied.IsSuccess());
		TestFalse(TEXT("(b) and saves the hero"), State->Hero().IsDirty());
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}));
	return true;
}
#endif
