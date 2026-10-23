// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The play tools against the player's live Enhanced
// Input state.
//   - HeldInput: a held key stays held while the session is paused between calls (no Completed, no time added to a
//     Hold trigger), and a 0.5 s Hold trigger fires once, after 0.5 s of game time.
//   - InputReports: paired axes (MouseX -> a Mouse2D mapping, Gamepad_LeftX -> Gamepad_Left2D), a key the player
//     remapped in the Enhanced Input user settings (the new key triggers, the asset's key warns why), query_play
//     what "input", play reset_input_settings, and an input_action value that does not fit the action's ValueType.

#include "CoreMinimal.h"
#include "Editor.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputDeveloperSettings.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "Kismet/GameplayStatics.h"
#include "LevelTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Play/PlayQuery.h"
#include "Play/PlaySession.h"
#include "PlayerMappableKeySettings.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "UserSettings/EnhancedInputUserSettings.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace PlayEnhancedInputTests
{
using namespace AssetDocumentLevelTests;
using UE::AssetDocumentLevel::Private::FLevelToolResult;
namespace Play = UE::AssetDocumentLevel::Private::Play;

FString Codes(const FLevelToolResult& Result)
{
	TArray<FString> Out;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Out.Add(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	return FString::Join(Out, TEXT(" | "));
}

const FAssetDocumentDiagnostic* Find(const FLevelToolResult& Result, const TCHAR* Code)
{
	return Result.FindDiagnostic(Code);
}

bool IsPending(const FLevelToolResult& Result, int32& OutId)
{
	const TSharedPtr<FJsonObject>* Pending = nullptr;
	if (Result.bSuccess && Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("pending"), Pending) && Pending != nullptr)
	{
		OutId = static_cast<int32>((*Pending)->GetNumberField(TEXT("id")));
		return true;
	}
	return false;
}

// One play request run to completion, polled every frame as the MCP tool polls it.
class FPlayCall final : public IAutomationLatentCommand
{
public:
	FPlayCall(FString InBody, TFunction<void(const FLevelToolResult&)> InOnDone)
		: Body(MoveTemp(InBody))
		, OnDone(MoveTemp(InOnDone))
	{
	}

	virtual bool Update() override
	{
		if (!bSent)
		{
			bSent = true;
			Started = FPlatformTime::Seconds();
			const FLevelToolResult Result = Play::RunPlayFromBody(Body);
			if (!IsPending(Result, Id))
			{
				OnDone(Result);
				return true;
			}
			return false;
		}
		const FLevelToolResult Result = Play::RunPlayFromBody(FString::Printf(TEXT("{\"action\": \"poll\", \"id\": %d}"), Id));
		int32 StillId = 0;
		if (IsPending(Result, StillId) && FPlatformTime::Seconds() - Started < 120.0)
		{
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
	double Started = 0.0;
};

// Real time passes while the game stays paused (the caller thinking between calls).
class FRealPause final : public IAutomationLatentCommand
{
public:
	explicit FRealPause(double InSeconds) : Seconds(InSeconds) {}

	virtual bool Update() override
	{
		if (Started < 0.0)
		{
			Started = FPlatformTime::Seconds();
		}
		return FPlatformTime::Seconds() - Started >= Seconds;
	}

private:
	double Seconds = 0.0;
	double Started = -1.0;
};

class FRunNow final : public IAutomationLatentCommand
{
public:
	explicit FRunNow(TFunction<void()> InWork) : Work(MoveTemp(InWork)) {}

	virtual bool Update() override
	{
		Work();
		return true;
	}

private:
	TFunction<void()> Work;
};

UInputAction* NewAction(const TCHAR* Name, EInputActionValueType Type)
{
	UInputAction* Action = NewObject<UInputAction>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UInputAction::StaticClass(), FName(Name)));
	Action->ValueType = Type;
	return Action;
}

UInputMappingContext* NewContext(const TCHAR* Name)
{
	return NewObject<UInputMappingContext>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UInputMappingContext::StaticClass(), FName(Name)));
}

UEnhancedInputLocalPlayerSubsystem* PlaySubsystem()
{
	UWorld* World = GEditor->PlayWorld;
	APlayerController* Controller = World != nullptr ? Play::GetLocalPlayerController(*World) : nullptr;
	return Controller != nullptr ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()) : nullptr;
}

void AddContext(UInputMappingContext* Context)
{
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = PlaySubsystem())
	{
		FModifyContextOptions Options;
		Options.bNotifyUserSettings = true;
		Options.bForceImmediately = true;
		Subsystem->AddMappingContext(Context, 0, Options);
	}
}

TSharedPtr<FJsonObject> ReportedAction(const FLevelToolResult& Result, const FString& ActionName)
{
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (!Result.Payload.IsValid() || !Result.Payload->TryGetArrayField(TEXT("inputs"), Inputs))
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Input : *Inputs)
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		if (!Input->AsObject()->TryGetArrayField(TEXT("actions"), Actions))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Action : *Actions)
		{
			if (Action->AsObject()->GetStringField(TEXT("action")) == ActionName)
			{
				return Action->AsObject();
			}
		}
	}
	return nullptr;
}

const FString SaveSlot = TEXT("EnhancedInputUserSettings");

void DeleteSavedUserSettings()
{
	const FString Slot = GetDefault<UEnhancedInputDeveloperSettings>()->InputSettingsSaveSlotName;
	for (const FString& Name : {SaveSlot, Slot})
	{
		if (UGameplayStatics::DoesSaveGameExist(Name, 0))
		{
			UGameplayStatics::DeleteGameInSlot(Name, 0);
		}
	}
}

void NewMap()
{
	TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
	GEditor->NewMap(false);
}

struct FHeldState
{
	TStrongObjectPtr<UInputAction> Sprint;
	TStrongObjectPtr<UInputAction> Hold;
	TStrongObjectPtr<UInputMappingContext> Context;
	TWeakObjectPtr<UEnhancedInputComponent> Component;
	int32 SprintTriggered = 0;
	int32 SprintCompleted = 0;
	int32 HoldTriggered = 0;
	// Game time at the start of the frame the hold began in (a trigger counts that whole frame), and at the end of
	// the frame it fired in.
	double HoldStartedAt = -1.0;
	double HoldTriggeredAt = -1.0;
	double FrameStart = 0.0;
	double LongestFrame = 0.0;
	float HoldElapsedBeforePause = -1.f;
	int32 HoldFiredBeforePause = 0;
	FDelegateHandle FrameHandle;
};

double PlayWorldTime()
{
	return GEditor->PlayWorld != nullptr ? GEditor->PlayWorld->GetTimeSeconds() : -1.0;
}

// How long the player's input has processed the action as held (its triggers' time).
float ActionElapsed(const UInputAction* Action)
{
	const UEnhancedInputLocalPlayerSubsystem* Subsystem = PlaySubsystem();
	const UEnhancedPlayerInput* PlayerInput = Subsystem != nullptr ? Subsystem->GetPlayerInput() : nullptr;
	const FInputActionInstance* Instance = PlayerInput != nullptr ? PlayerInput->FindActionInstanceData(Action) : nullptr;
	return Instance != nullptr ? Instance->GetElapsedTime() : -1.f;
}
} // namespace PlayEnhancedInputTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayHeldInputTest,
	"UEAssetDoc.Level.Play.HeldInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayHeldInputTest::RunTest(const FString& Parameters)
{
	using namespace PlayEnhancedInputTests;
	if (!TestNull(TEXT("no play session is running"), GEditor->PlayWorld.Get()))
	{
		return false;
	}
	NewMap();
	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("editor world"), World))
	{
		return false;
	}
	TSharedRef<FHeldState> State = MakeShared<FHeldState>();
	State->Sprint.Reset(NewAction(TEXT("IA_HeldSprint"), EInputActionValueType::Boolean));
	State->Hold.Reset(NewAction(TEXT("IA_HeldInteract"), EInputActionValueType::Boolean));
	UInputTriggerHold* Hold = NewObject<UInputTriggerHold>(State->Hold.Get());
	Hold->HoldTimeThreshold = 0.5f;
	Hold->bIsOneShot = true;
	State->Hold->Triggers.Add(Hold);
	State->Context.Reset(NewContext(TEXT("IMC_HeldInput")));
	State->Context->MapKey(State->Sprint.Get(), EKeys::LeftShift);
	State->Context->MapKey(State->Hold.Get(), EKeys::E);
	APawn* Pawn = Cast<APawn>(SpawnActor(*World->PersistentLevel, APawn::StaticClass(), FVector(0.0, 0.0, 300.0), TEXT("HeldInputPawn")));
	if (!TestNotNull(TEXT("pawn placed"), Pawn))
	{
		return false;
	}
	Pawn->AutoPossessPlayer = EAutoReceiveInput::Player0;

	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"start\", \"paused\": true}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("start (%s)"), *Codes(Result)), Result.bSuccess);
		UWorld* PlayWorld = GEditor->PlayWorld;
		APlayerController* Controller = PlayWorld != nullptr ? Play::GetLocalPlayerController(*PlayWorld) : nullptr;
		if (!TestNotNull(TEXT("player controller"), Controller))
		{
			return;
		}
		AddContext(State->Context.Get());
		TWeakPtr<FHeldState> WeakFrames = State;
		State->FrameHandle = FWorldDelegates::OnWorldTickStart.AddLambda([WeakFrames](UWorld* TickWorld, ELevelTick, float Delta)
		{
			TSharedPtr<FHeldState> Pinned = WeakFrames.Pin();
			if (Pinned.IsValid() && TickWorld == GEditor->PlayWorld)
			{
				Pinned->FrameStart = TickWorld->GetTimeSeconds();
				if (!TickWorld->bDebugPauseExecution)
				{
					Pinned->LongestFrame = FMath::Max(Pinned->LongestFrame, static_cast<double>(Delta));
				}
			}
		});
		UEnhancedInputComponent* Component = NewObject<UEnhancedInputComponent>(Controller, TEXT("HeldInputTestComponent"));
		Component->RegisterComponent();
		Controller->PushInputComponent(Component);
		State->Component = Component;
		TWeakPtr<FHeldState> Weak = State;
		Component->BindActionInstanceLambda(State->Sprint.Get(), ETriggerEvent::Triggered, [Weak](const FInputActionInstance&)
		{
			if (TSharedPtr<FHeldState> Pinned = Weak.Pin()) { ++Pinned->SprintTriggered; }
		});
		Component->BindActionInstanceLambda(State->Sprint.Get(), ETriggerEvent::Completed, [Weak](const FInputActionInstance&)
		{
			if (TSharedPtr<FHeldState> Pinned = Weak.Pin()) { ++Pinned->SprintCompleted; }
		});
		Component->BindActionInstanceLambda(State->Hold.Get(), ETriggerEvent::Started, [Weak](const FInputActionInstance&)
		{
			if (TSharedPtr<FHeldState> Pinned = Weak.Pin()) { Pinned->HoldStartedAt = Pinned->FrameStart; }
		});
		Component->BindActionInstanceLambda(State->Hold.Get(), ETriggerEvent::Triggered, [Weak](const FInputActionInstance&)
		{
			if (TSharedPtr<FHeldState> Pinned = Weak.Pin())
			{
				++Pinned->HoldTriggered;
				Pinned->HoldTriggeredAt = PlayWorldTime();
			}
		});
	}));
	// The game's first frames (long ones, while it warms up) pass before the keys go down.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"wait\", \"seconds\": 0.3}"), [this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("warm-up wait (%s)"), *Codes(Result)), Result.bSuccess);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"LeftShift\", \"state\": \"down\"}, ")
		TEXT("{\"key\": \"E\", \"state\": \"down\"}], \"settle\": 0.1}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("down (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(TEXT("sprint triggers while held"), State->SprintTriggered > 0);
		TestEqual(TEXT("sprint not completed"), State->SprintCompleted, 0);
		const double HeldSoFar = PlayWorldTime() - State->HoldStartedAt;
		TestEqual(*FString::Printf(TEXT("hold not yet triggered (%.3f s held)"), HeldSoFar), State->HoldTriggered,
			HeldSoFar >= 0.5 - KINDA_SMALL_NUMBER ? 1 : 0);
		TestTrue(TEXT("hold started"), State->HoldStartedAt >= 0.0);
		State->HoldElapsedBeforePause = ActionElapsed(State->Hold.Get());
		State->HoldFiredBeforePause = State->HoldTriggered;
	}));
	// The caller thinks for a second: the game stays paused and the keys stay held.
	ADD_LATENT_AUTOMATION_COMMAND(FRealPause(1.0));
	ADD_LATENT_AUTOMATION_COMMAND(FRunNow([this, State]()
	{
		TestEqual(TEXT("a pause between calls does not release the held key (no Completed)"), State->SprintCompleted, 0);
		TestEqual(TEXT("a pause between calls does not fire the Hold trigger"), State->HoldTriggered, State->HoldFiredBeforePause);
		TestEqual(TEXT("a pause between calls adds no time to the held action"), ActionElapsed(State->Hold.Get()), State->HoldElapsedBeforePause);
		State->SprintTriggered = 0;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"wait\", \"seconds\": 0.15}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("wait (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(TEXT("sprint keeps triggering after the pause"), State->SprintTriggered > 0);
		TestEqual(TEXT("sprint still not completed"), State->SprintCompleted, 0);
		const double HeldSoFar = PlayWorldTime() - State->HoldStartedAt;
		TestEqual(*FString::Printf(TEXT("the 0.5 s hold fires only after 0.5 s of game time (%.3f so far)"), HeldSoFar), State->HoldTriggered,
			HeldSoFar >= 0.5 - KINDA_SMALL_NUMBER ? 1 : 0);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"wait\", \"seconds\": 0.5}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("wait (%s)"), *Codes(Result)), Result.bSuccess);
		TestEqual(TEXT("the one-shot hold fired once"), State->HoldTriggered, 1);
		const double Held = State->HoldTriggeredAt - State->HoldStartedAt;
		// Game time advances a frame at a time: the hold fires in the frame its 0.5 s are reached.
		TestTrue(*FString::Printf(TEXT("after 0.5 s of game time (%.3f; frames up to %.3f s)"), Held, State->LongestFrame),
			Held >= 0.5 - KINDA_SMALL_NUMBER && Held <= 0.5 + State->LongestFrame + KINDA_SMALL_NUMBER);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"LeftShift\", \"state\": \"up\"}, ")
		TEXT("{\"key\": \"E\", \"state\": \"up\"}]}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("up (%s)"), *Codes(Result)), Result.bSuccess);
		TestEqual(TEXT("released: sprint Completed once"), State->SprintCompleted, 1);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"stop\"}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop (%s)"), *Codes(Result)), Result.bSuccess);
		FWorldDelegates::OnWorldTickStart.Remove(State->FrameHandle);
		NewMap();
	}));
	return true;
}

namespace PlayEnhancedInputTests
{
struct FReportState
{
	TStrongObjectPtr<UInputAction> Look;
	TStrongObjectPtr<UInputAction> Move;
	TStrongObjectPtr<UInputAction> Jump;
	TStrongObjectPtr<UInputMappingContext> Context;
	bool bUserSettingsWere = true;
};

TSharedPtr<FJsonObject> QueriedMapping(const FLevelToolResult& Result, const FString& ContextPath, const FString& ActionName)
{
	if (!Result.bSuccess || !Result.Payload.IsValid())
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Context : Result.Payload->GetArrayField(TEXT("contexts")))
	{
		if (Context->AsObject()->GetStringField(TEXT("context")) != ContextPath)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Mapping : Context->AsObject()->GetArrayField(TEXT("mappings")))
		{
			if (Mapping->AsObject()->GetStringField(TEXT("action")) == ActionName)
			{
				return Mapping->AsObject();
			}
		}
	}
	return nullptr;
}
} // namespace PlayEnhancedInputTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayInputReportsTest,
	"UEAssetDoc.Level.Play.InputReports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayInputReportsTest::RunTest(const FString& Parameters)
{
	using namespace PlayEnhancedInputTests;
	if (!TestNull(TEXT("no play session is running"), GEditor->PlayWorld.Get()))
	{
		return false;
	}
	NewMap();
	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("editor world"), World))
	{
		return false;
	}
	TSharedRef<FReportState> State = MakeShared<FReportState>();
	UEnhancedInputDeveloperSettings* Settings = GetMutableDefault<UEnhancedInputDeveloperSettings>();
	State->bUserSettingsWere = Settings->bEnableUserSettings;
	Settings->bEnableUserSettings = true;
	DeleteSavedUserSettings();
	State->Look.Reset(NewAction(TEXT("IA_ReportLook"), EInputActionValueType::Axis2D));
	State->Move.Reset(NewAction(TEXT("IA_ReportMove"), EInputActionValueType::Axis2D));
	State->Jump.Reset(NewAction(TEXT("IA_ReportJump"), EInputActionValueType::Boolean));
	if (FObjectProperty* Property = FindFProperty<FObjectProperty>(UInputAction::StaticClass(), TEXT("PlayerMappableKeySettings")))
	{
		UPlayerMappableKeySettings* Mappable = NewObject<UPlayerMappableKeySettings>(State->Jump.Get());
		Mappable->Name = TEXT("RemapJump");
		Mappable->DisplayName = FText::FromString(TEXT("Jump"));
		Property->SetObjectPropertyValue_InContainer(State->Jump.Get(), Mappable);
	}
	State->Context.Reset(NewContext(TEXT("IMC_Report")));
	State->Context->MapKey(State->Look.Get(), EKeys::Mouse2D);
	State->Context->MapKey(State->Move.Get(), EKeys::Gamepad_Left2D);
	State->Context->MapKey(State->Jump.Get(), EKeys::SpaceBar);
	APawn* Pawn = Cast<APawn>(SpawnActor(*World->PersistentLevel, APawn::StaticClass(), FVector(0.0, 0.0, 300.0), TEXT("InputReportPawn")));
	if (!TestNotNull(TEXT("pawn placed"), Pawn))
	{
		Settings->bEnableUserSettings = State->bUserSettingsWere;
		return false;
	}
	Pawn->AutoPossessPlayer = EAutoReceiveInput::Player0;
	const FString LookName = State->Look->GetName();
	const FString MoveName = State->Move->GetName();
	const FString JumpName = State->Jump->GetName();
	const FString ContextPath = State->Context->GetPathName();

	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"start\", \"paused\": true}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("start (%s)"), *Codes(Result)), Result.bSuccess);
		TestNotNull(TEXT("input subsystem"), PlaySubsystem());
		AddContext(State->Context.Get());
	}));
	// Item 5: one half of a paired axis reaches the 2D mapping.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"MouseX\", \"value\": 5, \"duration\": 0.1}], \"settle\": 0.05}"),
		[this, LookName](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("MouseX (%s)"), *Codes(Result)), Result.bSuccess);
		TestNull(TEXT("MouseX is not reported unmapped"), Find(Result, TEXT("Play.KeyNotMapped")));
		const TSharedPtr<FJsonObject> Look = ReportedAction(Result, LookName);
		TestTrue(TEXT("MouseX triggers the Mouse2D action"), Look.IsValid() && Look->GetBoolField(TEXT("triggered")));
		TestTrue(TEXT("via Mouse2D"), Look.IsValid() && Look->GetStringField(TEXT("via")) == TEXT("Mouse2D"));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"Gamepad_LeftX\", \"value\": 1, \"duration\": 0.1}], \"settle\": 0.05}"),
		[this, MoveName](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("Gamepad_LeftX (%s)"), *Codes(Result)), Result.bSuccess);
		TestNull(TEXT("Gamepad_LeftX is not reported unmapped"), Find(Result, TEXT("Play.KeyNotMapped")));
		const TSharedPtr<FJsonObject> Move = ReportedAction(Result, MoveName);
		TestTrue(TEXT("Gamepad_LeftX triggers the stick action"), Move.IsValid() && Move->GetBoolField(TEXT("triggered")));
		TestTrue(TEXT("via Gamepad_Left2D"), Move.IsValid() && Move->GetStringField(TEXT("via")) == TEXT("Gamepad_Left2D"));
	}));
	// Item 4: the player remaps Jump to G in the Enhanced Input user settings (saved, as a settings menu does).
	ADD_LATENT_AUTOMATION_COMMAND(FRunNow([this]()
	{
		UEnhancedInputLocalPlayerSubsystem* Subsystem = PlaySubsystem();
		UEnhancedInputUserSettings* UserSettings = Subsystem != nullptr ? Subsystem->GetUserSettings() : nullptr;
		if (!TestNotNull(TEXT("user settings"), UserSettings))
		{
			return;
		}
		FMapPlayerKeyArgs Args;
		Args.MappingName = TEXT("RemapJump");
		Args.Slot = EPlayerMappableKeySlot::First;
		Args.NewKey = EKeys::G;
		FGameplayTagContainer Failure;
		UserSettings->MapPlayerKey(Args, Failure);
		TestEqual(TEXT("remapped (no failure reason)"), Failure.Num(), 0);
		UserSettings->ApplySettings();
		UserSettings->SaveSettings();
		FModifyContextOptions Options;
		Options.bForceImmediately = true;
		Subsystem->RequestRebuildControlMappings(Options);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"G\", \"duration\": 0.1}], \"settle\": 0.05}"),
		[this, JumpName](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("G (%s)"), *Codes(Result)), Result.bSuccess);
		TestNull(TEXT("the remapped key is not reported unmapped"), Find(Result, TEXT("Play.KeyNotMapped")));
		const TSharedPtr<FJsonObject> Jump = ReportedAction(Result, JumpName);
		TestTrue(TEXT("G triggers Jump"), Jump.IsValid() && Jump->GetBoolField(TEXT("triggered")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"SpaceBar\", \"duration\": 0.1}], \"settle\": 0.05}"),
		[this](const FLevelToolResult& Result)
	{
		const FAssetDocumentDiagnostic* NotMapped = Find(Result, TEXT("Play.KeyNotMapped"));
		TestTrue(*FString::Printf(TEXT("the asset's key warns that the player's key settings moved it (%s)"), *Codes(Result)),
			NotMapped != nullptr && NotMapped->Message.Contains(TEXT("key settings")) && NotMapped->Message.Contains(TEXT("G")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FRunNow([this, ContextPath, JumpName, LookName]()
	{
		const FLevelToolResult Input = Play::RunPlayQueryFromBody(TEXT("{\"what\": \"input\"}"));
		TestTrue(*FString::Printf(TEXT("query_play input (%s)"), *Codes(Input)), Input.bSuccess);
		const TSharedPtr<FJsonObject> Jump = QueriedMapping(Input, ContextPath, JumpName);
		TestTrue(TEXT("Jump's effective key is G, the asset's SpaceBar"), Jump.IsValid() && Jump->GetStringField(TEXT("key")) == TEXT("G")
			&& Jump->GetStringField(TEXT("asset_key")) == TEXT("SpaceBar"));
		const TSharedPtr<FJsonObject> Look = QueriedMapping(Input, ContextPath, LookName);
		TestTrue(TEXT("Look keeps Mouse2D"), Look.IsValid() && Look->GetStringField(TEXT("key")) == TEXT("Mouse2D") && !Look->HasField(TEXT("asset_key")));
		const TSharedPtr<FJsonObject>* User = nullptr;
		if (TestTrue(TEXT("user settings reported"), Input.Payload.IsValid() && Input.Payload->TryGetObjectField(TEXT("user_settings"), User)))
		{
			TestTrue(TEXT("the settings are saved"), (*User)->GetBoolField(TEXT("saved")));
			const TArray<TSharedPtr<FJsonValue>>& Customized = (*User)->GetArrayField(TEXT("customized"));
			TestTrue(TEXT("the customized mapping"), Customized.Num() == 1
				&& Customized[0]->AsObject()->GetStringField(TEXT("mapping")) == TEXT("RemapJump")
				&& Customized[0]->AsObject()->GetStringField(TEXT("default_key")) == TEXT("SpaceBar")
				&& Customized[0]->AsObject()->GetStringField(TEXT("key")) == TEXT("G"));
		}
		TestNotNull(TEXT("the override is a warning"), Find(Input, TEXT("Play.InputUserSettingsOverride")));
		TestTrue(TEXT("query_play input takes no other field"), !Play::RunPlayQueryFromBody(TEXT("{\"what\": \"input\", \"class\": \"Pawn\"}")).bSuccess);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"reset_input_settings\"}"), [this, ContextPath, JumpName](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("reset (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(TEXT("the saved file is deleted"), Result.Payload.IsValid() && Result.Payload->GetBoolField(TEXT("deleted")));
		const FLevelToolResult Input = Play::RunPlayQueryFromBody(TEXT("{\"what\": \"input\"}"));
		const TSharedPtr<FJsonObject> Jump = QueriedMapping(Input, ContextPath, JumpName);
		TestTrue(TEXT("Jump is back on SpaceBar"), Jump.IsValid() && Jump->GetStringField(TEXT("key")) == TEXT("SpaceBar") && !Jump->HasField(TEXT("asset_key")));
		const TSharedPtr<FJsonObject>* User = nullptr;
		TestTrue(TEXT("nothing customized or saved"), Input.Payload.IsValid() && Input.Payload->TryGetObjectField(TEXT("user_settings"), User)
			&& !(*User)->GetBoolField(TEXT("saved")) && (*User)->GetArrayField(TEXT("customized")).IsEmpty());
		TestNull(TEXT("no override warning"), Find(Input, TEXT("Play.InputUserSettingsOverride")));
	}));
	// Item 3: an input_action value that does not fit the action's ValueType is an error naming the type.
	ADD_LATENT_AUTOMATION_COMMAND(FRunNow([this, State]()
	{
		const FLevelToolResult Vector = Play::RunPlayFromBody(FString::Printf(
			TEXT("{\"action\": \"input\", \"inputs\": [{\"input_action\": \"%s\", \"value\": {\"X\": 0, \"Y\": 1}}]}"), *State->Jump->GetPathName()));
		const FAssetDocumentDiagnostic* Mismatch = Find(Vector, TEXT("Play.InputValueTypeMismatch"));
		TestTrue(*FString::Printf(TEXT("a 2D value for a Boolean action (%s)"), *Codes(Vector)), !Vector.bSuccess && Mismatch != nullptr
			&& Mismatch->Message.Contains(TEXT("Boolean")));
		const FLevelToolResult Scalar = Play::RunPlayFromBody(FString::Printf(
			TEXT("{\"action\": \"input\", \"inputs\": [{\"input_action\": \"%s\", \"value\": 1}]}"), *State->Look->GetPathName()));
		Mismatch = Find(Scalar, TEXT("Play.InputValueTypeMismatch"));
		TestTrue(*FString::Printf(TEXT("a number for an Axis2D action (%s)"), *Codes(Scalar)), !Scalar.bSuccess && Mismatch != nullptr
			&& Mismatch->Message.Contains(TEXT("Axis2D")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayCall(TEXT("{\"action\": \"stop\"}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop (%s)"), *Codes(Result)), Result.bSuccess);
		DeleteSavedUserSettings();
		GetMutableDefault<UEnhancedInputDeveloperSettings>()->bEnableUserSettings = State->bUserSettingsWere;
		NewMap();
	}));
	return true;
}
#endif
