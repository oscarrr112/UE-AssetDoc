// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Play/PlaySession.h"

#include "AssetDocumentViewportCapture.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Blueprint.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/PlatformTime.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputKeyEventArgs.h"
#include "InputMappingContext.h"
#include "IAssetViewport.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "LevelMapTools.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Modules/ModuleManager.h"
#include "Play/PlayActions.h"
#include "Play/PlayInput.h"
#include "Play/PlayLog.h"
#include "Play/PlayQuery.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UnrealEdGlobals.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::AssetDocumentLevel::Private::Play
{
namespace PlaySessionLocal
{
FString ValueTypeName(EInputActionValueType Type)
{
	const UEnum* Enum = StaticEnum<EInputActionValueType>();
	return Enum != nullptr ? Enum->GetNameStringByValue(static_cast<int64>(Type)) : FString::FromInt(static_cast<int32>(Type));
}

// An input_action value against the action's ValueType: the shape it takes ("true/false or a number"), or empty when
// Value fits. A value the action would silently cut (Y of a Boolean action, a number for a 2D action) does not fit.
FString ActionValueMismatch(const UInputAction& Action, const FJsonValue& Value, const FVector& Vector)
{
	const bool bScalar = Value.Type == EJson::Number || Value.Type == EJson::Boolean;
	switch (Action.ValueType)
	{
	case EInputActionValueType::Boolean:
	case EInputActionValueType::Axis1D:
		return !bScalar && (!FMath::IsNearlyZero(Vector.Y) || !FMath::IsNearlyZero(Vector.Z)) ? FString(TEXT("true/false or a number")) : FString();
	case EInputActionValueType::Axis2D:
		return bScalar || !FMath::IsNearlyZero(Vector.Z) ? FString(TEXT("{\"X\", \"Y\"}")) : FString();
	case EInputActionValueType::Axis3D:
		return bScalar ? FString(TEXT("{\"X\", \"Y\", \"Z\"}")) : FString();
	default:
		return FString();
	}
}
}

namespace
{
constexpr double DefaultWaitTimeout = 10.0;
constexpr double MaximumTimeout = 60.0;
constexpr double StartTimeout = 60.0;
constexpr double StopTimeout = 30.0;
constexpr int32 PollMs = 50;
constexpr int32 MaxInputs = 50;
constexpr double MaxInputSeconds = 60.0;

double Round3(double Value)
{
	return FMath::RoundToDouble(Value * 1000.0) / 1000.0;
}

FAssetDocumentDiagnostic MakeWarning(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr)
{
	FAssetDocumentDiagnostic Diagnostic = MakePlayDiagnostic(Path, Code, Message, MoveTemp(Details));
	Diagnostic.Severity = TEXT("warning");
	return Diagnostic;
}

FString TriggerEventName(ETriggerEvent Event)
{
	switch (Event)
	{
	case ETriggerEvent::Triggered: return TEXT("Triggered");
	case ETriggerEvent::Started: return TEXT("Started");
	case ETriggerEvent::Ongoing: return TEXT("Ongoing");
	case ETriggerEvent::Canceled: return TEXT("Canceled");
	case ETriggerEvent::Completed: return TEXT("Completed");
	default: return TEXT("None");
	}
}

TSharedRef<FJsonObject> ActionValueJson(const FInputActionValue& Value)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	const FVector Vector = Value.Get<FVector>();
	Json->SetNumberField(TEXT("X"), Round3(Vector.X));
	Json->SetNumberField(TEXT("Y"), Round3(Vector.Y));
	Json->SetNumberField(TEXT("Z"), Round3(Vector.Z));
	return Json;
}

// One scheduled input of a play "input" request.
struct FScheduledInput
{
	enum class EState : uint8
	{
		Scheduled,
		Active,
		Released,
		Done,
	};
	// Tap: press, hold for duration, release (default). Down: press and keep it held after the request
	// (until a later state "up"). Up: release an input a "down" left held.
	enum class EMode : uint8
	{
		Tap,
		Down,
		Up,
	};

	int32 Index = 0;
	EMode Mode = EMode::Tap;
	// The held-input identity: the action's path or the key's name.
	FString HeldId;
	// Down: the input was left held when the request ended.
	bool bKeptDown = false;
	TStrongObjectPtr<UInputAction> Action;
	FInputActionValue ActionValue;
	FKey Key;
	FVector KeyValue = FVector(1.0, 0.0, 0.0);
	bool bAnalog = false;
	double At = 0.0;
	double Duration = 0.0;

	EState State = EState::Scheduled;
	double StartWorld = 0.0;
	double EndWorld = 0.0;
	double ActualStart = 0.0;
	double ActualEnd = 0.0;
	int32 ActiveTicks = 0;
	int32 ReleasedTicks = 0;

	// What the input reached: watched actions (the action itself, or the actions the key is mapped
	// to in the player's active mapping contexts) and what they did.
	struct FWatched
	{
		TWeakObjectPtr<const UInputAction> Action;
		// The paired-axis mapping key a key input reached the action through (MouseX -> Mouse2D).
		FString Via;
		TSet<FString> Events;
		// The action's last trigger time before the input began: a later one is this input's.
		float TriggeredBefore = -1.0f;
		ETriggerEvent LastEvent = ETriggerEvent::None;
		int32 TriggeredTicks = 0;
		bool bTriggered = false;
	};
	TArray<FWatched> Watched;
	TArray<FString> MappedKeys;
	TArray<FString> Listeners;
};

enum class EOperationKind : uint8
{
	Start,
	Stop,
	Wait,
	Input,
};

struct FOperation
{
	int32 Id = 0;
	EOperationKind Kind = EOperationKind::Start;
	double StartReal = 0.0;
	double DeadlineReal = 0.0;
	uint64 LogSeq = 0;
	bool bDone = false;
	FLevelToolResult Result;
	TArray<FAssetDocumentDiagnostic> Warnings;

	// Start
	FString Mode;
	bool bSimulate = false;
	bool bStartPaused = false;
	// Paused before the world's first tick (when the editor reported the session started).
	bool bPausedAtStart = false;
	double BegunReal = 0.0;
	TArray<FString> StartCommands;
	TArray<TSharedPtr<FJsonValue>> StartConsole;

	// Stop
	double SessionRealSeconds = 0.0;
	double LastWorldTime = 0.0;

	// Wait
	double StartWorld = 0.0;
	TOptional<double> UntilWorld;
	TArray<FPlayCondition> Conditions;
	bool bRepause = false;

	// Input
	TArray<FScheduledInput> Inputs;
	double Settle = 0.0;
};

TSharedRef<FJsonObject> LogSummaryJson(uint64 Since, double BaseTime)
{
	FlushPendingLog();
	int32 Errors = 0;
	int32 Warnings = 0;
	TArray<FPlayLogEntry> Lines;
	FPlayLog::Get().Summarize(Since, 10, Errors, Warnings, Lines);
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("errors"), Errors);
	Json->SetNumberField(TEXT("warnings"), Warnings);
	TArray<TSharedPtr<FJsonValue>> LineValues;
	for (const FPlayLogEntry& Entry : Lines)
	{
		LineValues.Add(MakeShared<FJsonValueObject>(LogEntryJson(Entry, BaseTime, 400)));
	}
	if (!LineValues.IsEmpty())
	{
		Json->SetArrayField(TEXT("lines"), LineValues);
	}
	FPlayLogFilter PrintFilter;
	PrintFilter.Since = Since;
	PrintFilter.Categories.Add(TEXT("LogBlueprintUserMessages"));
	int32 Skipped = 0;
	uint64 Oldest = 0;
	const TArray<FPlayLogEntry> Prints = FPlayLog::Get().Query(PrintFilter, 10, Skipped, Oldest);
	if (!Prints.IsEmpty())
	{
		TArray<FString> PrintLines;
		for (const FPlayLogEntry& Entry : Prints)
		{
			PrintLines.Add(Entry.Message.Left(300));
		}
		Json->SetArrayField(TEXT("print"), PlayStringArray(PrintLines));
		if (Skipped > 0)
		{
			Json->SetNumberField(TEXT("print_skipped"), Skipped);
		}
	}
	Json->SetNumberField(TEXT("last_seq"), static_cast<double>(FPlayLog::Get().GetLastSeq()));
	return Json;
}

// An input a play "input" with state "down" left held; it stays held across calls until state "up" or stop.
struct FHeldInput
{
	TStrongObjectPtr<UInputAction> Action;
	FInputActionValue ActionValue;
	FKey Key;
	FVector KeyValue = FVector(1.0, 0.0, 0.0);
	bool bAnalog = false;
	double Since = 0.0;
};

class FPlayManager
{
public:
	static FPlayManager& Get()
	{
		static FPlayManager Instance;
		return Instance;
	}

	void Startup()
	{
		FPlayLog::Get().Register();
		TickStartHandle = FWorldDelegates::OnWorldTickStart.AddRaw(this, &FPlayManager::OnWorldTickStart);
		PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FPlayManager::OnWorldPostActorTick);
		PreBeginPieHandle = FEditorDelegates::PreBeginPIE.AddRaw(this, &FPlayManager::OnPreBeginPie);
		PostPieStartedHandle = FEditorDelegates::PostPIEStarted.AddRaw(this, &FPlayManager::OnPostPieStarted);
		EndPieHandle = FEditorDelegates::EndPIE.AddRaw(this, &FPlayManager::OnEndPie);
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FPlayManager::OnCoreTick));
		if (GEditor != nullptr)
		{
			UEditorEngine::FShouldDisableCPUThrottling Delegate = UEditorEngine::FShouldDisableCPUThrottling::CreateRaw(this, &FPlayManager::ShouldDisableThrottling);
			ThrottleHandle = Delegate.GetHandle();
			GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Delegate));
		}
	}

	void Shutdown()
	{
		FWorldDelegates::OnWorldTickStart.Remove(TickStartHandle);
		FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
		FEditorDelegates::PreBeginPIE.Remove(PreBeginPieHandle);
		FEditorDelegates::PostPIEStarted.Remove(PostPieStartedHandle);
		FEditorDelegates::EndPIE.Remove(EndPieHandle);
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		if (GEditor != nullptr)
		{
			const FDelegateHandle Handle = ThrottleHandle;
			GEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll([Handle](const UEditorEngine::FShouldDisableCPUThrottling& Delegate)
			{
				return Delegate.GetHandle() == Handle;
			});
		}
		Operations.Reset();
		HeldInputs.Reset();
		bInputFrozen = false;
		FrozenController.Reset();
		FPlayLog::Get().Unregister();
	}

	FLevelToolResult Run(const TSharedPtr<FJsonObject>& Request);

	TSharedRef<FJsonObject> Status(int32 FinishingId = 0) const;

	void GetLogBase(uint64& OutSeq, double& OutTime) const
	{
		OutSeq = SessionLogSeq;
		OutTime = SessionStartReal;
	}

private:
	FLevelToolResult Start(const FJsonObject& Request);
	FLevelToolResult Stop(const FJsonObject& Request);
	FLevelToolResult Pause(bool bPause);
	FLevelToolResult Wait(const FJsonObject& Request);
	FLevelToolResult HandleInput(const FJsonObject& Request);
	FLevelToolResult Console(const FJsonObject& Request);
	FLevelToolResult CallOrSet(const FJsonObject& Request, bool bCall);
	FLevelToolResult Poll(const FJsonObject& Request);
	FLevelToolResult ResetInputSettings(const FJsonObject& Request);
	// Runs one console command as the local player types it (ke targets resolved in the play world); the
	// result {command, output}; a refused command adds a diagnostic and returns null.
	TSharedPtr<FJsonObject> RunConsoleCommand(UWorld& World, APlayerController& Controller, const FString& Command, const FString& Path,
		TArray<FAssetDocumentDiagnostic>& OutDiagnostics);

	TSharedPtr<FOperation> AddOperation(EOperationKind Kind, double Timeout);
	FLevelToolResult PendingResult(const FOperation& Operation, const FString& Message) const;
	void Finish(FOperation& Operation, FLevelToolResult Result);
	TSharedRef<FJsonObject> ResultPayload(const FOperation& Operation) const;

	void UpdateOperations(UWorld* World, bool bPostTick);
	void UpdateStart(FOperation& Operation);
	void UpdateStop(FOperation& Operation);
	void UpdateWait(FOperation& Operation, UWorld* World, bool bPostTick);
	void UpdateInput(FOperation& Operation, UWorld* World);
	void CompleteWait(FOperation& Operation, UWorld* World, const FString& Reason, int32 Condition, TSharedPtr<FJsonValue> Observed);

	void BeginInput(FScheduledInput& Input, APlayerController& Controller, double Now);
	void ReleaseInput(FScheduledInput& Input, APlayerController* Controller);
	void ReleaseAllInputs(APlayerController* Controller);
	void ReleaseHeld(const FHeldInput& Held, APlayerController& Controller);
	void ReleaseAllHeld(APlayerController* Controller);
	// A held input stays held while the game is paused between calls, as for a human holding a key: the player's
	// input is not evaluated in paused frames (Enhanced Input would end every action that does not trigger when
	// paused and let timed triggers count real time), and resumes with the game.
	void UpdateHeldInputFreeze(UWorld& World);
	void FreezeHeldInputs(APlayerController& Controller);
	void ThawHeldInputs();

	void OnWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void OnWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void OnPreBeginPie(bool bSimulating);
	void OnPostPieStarted(bool bSimulating);
	void OnEndPie(bool bSimulating);
	bool OnCoreTick(float DeltaTime);
	bool ShouldDisableThrottling() const
	{
		return bToolSession && GEditor != nullptr && GEditor->PlayWorld != nullptr;
	}

	TMap<int32, TSharedPtr<FOperation>> Operations;
	TMap<FString, FHeldInput> HeldInputs;
	// UpdateHeldInputFreeze: the controller whose input is frozen and its own bTickEvenWhenPaused.
	TWeakObjectPtr<APlayerController> FrozenController;
	bool bInputFrozen = false;
	bool bFrozenTickEvenWhenPaused = true;
	int32 NextId = 1;
	uint64 SessionLogSeq = 0;
	double SessionStartReal = 0.0;
	// Real seconds the session's world ran unpaused (real_time minus the time it stood paused).
	double UnpausedReal = 0.0;
	double LastCoreTickReal = 0.0;
	FString SessionMode;
	bool bToolSession = false;
	FDelegateHandle TickStartHandle;
	FDelegateHandle PostActorTickHandle;
	FDelegateHandle PreBeginPieHandle;
	FDelegateHandle PostPieStartedHandle;
	FDelegateHandle EndPieHandle;
	FDelegateHandle ThrottleHandle;
	FTSTicker::FDelegateHandle TickerHandle;
};

UEnhancedInputLocalPlayerSubsystem* GetInputSubsystem(APlayerController& Controller)
{
	return ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller.GetLocalPlayer());
}

FString SessionState()
{
	if (GEditor == nullptr)
	{
		return TEXT("stopped");
	}
	UWorld* World = GEditor->PlayWorld;
	if (World == nullptr)
	{
		return GEditor->IsPlaySessionRequestQueued() ? TEXT("starting") : TEXT("stopped");
	}
	if (GEditor->ShouldEndPlayMap())
	{
		return TEXT("stopping");
	}
	if (!World->HasBegunPlay())
	{
		return TEXT("starting");
	}
	if (World->bDebugPauseExecution)
	{
		return TEXT("paused");
	}
	return GEditor->IsSimulatingInEditor() ? TEXT("simulating") : TEXT("playing");
}

TSharedRef<FJsonObject> FPlayManager::Status(int32 FinishingId) const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	const FString State = SessionState();
	Json->SetStringField(TEXT("state"), State);
	UWorld* World = GetPlayWorld();
	if (World != nullptr)
	{
		if (!SessionMode.IsEmpty())
		{
			Json->SetStringField(TEXT("mode"), SessionMode);
		}
		Json->SetStringField(TEXT("world"), World->GetPathName());
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			Json->SetStringField(TEXT("map"), EditorWorld->GetOutermost()->GetName());
		}
		Json->SetNumberField(TEXT("world_time"), Round3(World->GetTimeSeconds()));
		// real_time: wall-clock seconds since the session started, paused time and the time between calls included;
		// unpaused_real_time: only the real seconds the world ran (what a timer in real time saw).
		Json->SetNumberField(TEXT("real_time"), Round3(FPlatformTime::Seconds() - SessionStartReal));
		Json->SetNumberField(TEXT("unpaused_real_time"), Round3(UnpausedReal));
		if (AWorldSettings* Settings = World->GetWorldSettings())
		{
			if (!FMath::IsNearlyEqual(Settings->GetEffectiveTimeDilation(), 1.0f))
			{
				Json->SetNumberField(TEXT("time_dilation"), Settings->GetEffectiveTimeDilation());
			}
		}
		if (const AGameModeBase* GameMode = World->GetAuthGameMode())
		{
			Json->SetStringField(TEXT("game_mode"), GameMode->GetClass()->GetPathName());
		}
		if (APlayerController* Controller = GetLocalPlayerController(*World))
		{
			TSharedRef<FJsonObject> Player = MakeShared<FJsonObject>();
			Player->SetStringField(TEXT("controller"), Controller->GetName());
			if (APawn* Pawn = Controller->GetPawn())
			{
				FPlayActorJsonOptions Options;
				Player->SetObjectField(TEXT("pawn"), PlayActorJson(*Pawn, Options));
			}
			else
			{
				Player->SetField(TEXT("pawn"), MakeShared<FJsonValueNull>());
			}
			Json->SetObjectField(TEXT("player"), Player);
		}
		if (!HeldInputs.IsEmpty())
		{
			TArray<FString> Held;
			HeldInputs.GenerateKeyArray(Held);
			Held.Sort();
			Json->SetArrayField(TEXT("held_inputs"), PlayStringArray(Held));
		}
	}
	if (SessionStartReal > 0.0)
	{
		FlushPendingLog();
		int32 Errors = 0;
		int32 Warnings = 0;
		TArray<FPlayLogEntry> Ignored;
		FPlayLog::Get().Summarize(SessionLogSeq, 0, Errors, Warnings, Ignored);
		TSharedRef<FJsonObject> Log = MakeShared<FJsonObject>();
		Log->SetNumberField(TEXT("session_errors"), Errors);
		Log->SetNumberField(TEXT("session_warnings"), Warnings);
		Log->SetNumberField(TEXT("session_start_seq"), static_cast<double>(SessionLogSeq));
		Log->SetNumberField(TEXT("last_seq"), static_cast<double>(FPlayLog::Get().GetLastSeq()));
		Json->SetObjectField(TEXT("log"), Log);
	}
	int32 Pending = 0;
	for (const TPair<int32, TSharedPtr<FOperation>>& Entry : Operations)
	{
		Pending += Entry.Value->bDone || Entry.Key == FinishingId ? 0 : 1;
	}
	if (Pending > 0)
	{
		Json->SetNumberField(TEXT("pending_operations"), Pending);
	}
	return Json;
}

TSharedPtr<FOperation> FPlayManager::AddOperation(EOperationKind Kind, double Timeout)
{
	TSharedPtr<FOperation> Operation = MakeShared<FOperation>();
	Operation->Id = NextId++;
	Operation->Kind = Kind;
	Operation->StartReal = FPlatformTime::Seconds();
	Operation->DeadlineReal = Operation->StartReal + Timeout;
	FlushPendingLog();
	Operation->LogSeq = FPlayLog::Get().GetLastSeq();
	Operations.Add(Operation->Id, Operation);
	return Operation;
}

TSharedRef<FJsonObject> FPlayManager::ResultPayload(const FOperation& Operation) const
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetObjectField(TEXT("session"), Status(Operation.Id));
	Payload->SetObjectField(TEXT("log"), LogSummaryJson(Operation.LogSeq, SessionStartReal));
	return Payload;
}

FLevelToolResult FPlayManager::PendingResult(const FOperation& Operation, const FString& Message) const
{
	TSharedRef<FJsonObject> Pending = MakeShared<FJsonObject>();
	Pending->SetNumberField(TEXT("id"), Operation.Id);
	Pending->SetNumberField(TEXT("poll_ms"), PollMs);
	Pending->SetNumberField(TEXT("timeout_s"), Round3(Operation.DeadlineReal - Operation.StartReal));
	Pending->SetNumberField(TEXT("elapsed_s"), Round3(FPlatformTime::Seconds() - Operation.StartReal));
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetObjectField(TEXT("pending"), Pending);
	FLevelToolResult Result = FLevelToolResult::Success(Message, Payload);
	Result.Diagnostics = Operation.Warnings;
	return Result;
}

void FPlayManager::Finish(FOperation& Operation, FLevelToolResult Result)
{
	if (Operation.bDone)
	{
		return;
	}
	for (const FAssetDocumentDiagnostic& Diagnostic : Operation.Warnings)
	{
		Result.Diagnostics.Add(Diagnostic);
	}
	Operation.Result = MoveTemp(Result);
	Operation.bDone = true;
}

FLevelToolResult FPlayManager::Poll(const FJsonObject& Request)
{
	double IdNumber = 0.0;
	if (!Request.TryGetNumberField(TEXT("id"), IdNumber))
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(PlayInvalidRequest(TEXT("/id"), TEXT("poll needs the 'id' a pending result returned.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	const int32 Id = static_cast<int32>(IdNumber);
	TSharedPtr<FOperation>* Found = Operations.Find(Id);
	if (Found == nullptr)
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/id"), TEXT("Play.UnknownOperation"),
			FString::Printf(TEXT("No pending play operation %d (finished results are returned once)."), Id)));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	TSharedPtr<FOperation> Operation = *Found;
	UpdateOperations(GetPlayWorld(), false);
	if (!Operation->bDone)
	{
		return PendingResult(*Operation, TEXT("Still running; poll again."));
	}
	Operations.Remove(Id);
	return Operation->Result;
}

void FPlayManager::UpdateOperations(UWorld* World, bool bPostTick)
{
	TArray<TSharedPtr<FOperation>> Current;
	Operations.GenerateValueArray(Current);
	for (const TSharedPtr<FOperation>& Operation : Current)
	{
		if (Operation->bDone)
		{
			continue;
		}
		switch (Operation->Kind)
		{
		case EOperationKind::Start: UpdateStart(*Operation); break;
		case EOperationKind::Stop: UpdateStop(*Operation); break;
		case EOperationKind::Wait: UpdateWait(*Operation, World, bPostTick); break;
		case EOperationKind::Input: UpdateInput(*Operation, World); break;
		default: break;
		}
	}
	// Results nobody collected are dropped after a minute.
	const double Now = FPlatformTime::Seconds();
	for (auto It = Operations.CreateIterator(); It; ++It)
	{
		if (It->Value->bDone && Now - It->Value->DeadlineReal > 60.0)
		{
			It.RemoveCurrent();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// start / stop / pause

struct FBlueprintErrors
{
	TWeakObjectPtr<UBlueprint> Blueprint;
	TArray<FString> Messages;
};

void CollectMessages(const FCompilerResultsLog& Log, TArray<FString>& Out)
{
	for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
	{
		if (Message->GetSeverity() == EMessageSeverity::Error && Out.Num() < 5)
		{
			Out.Add(Message->ToText().ToString());
		}
	}
}

// The editor compiles modified Blueprints before Play and asks the user what to do about Blueprints
// with compile errors. This does the compile up front (as the editor would) so that neither prompt
// can appear, and returns the Blueprints that have errors.
TArray<FBlueprintErrors> CompileForPlay()
{
	TArray<UBlueprint*> ToCompile;
	TArray<UBlueprint*> Errored;
	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		UBlueprint* Blueprint = *It;
		if (!IsValid(Blueprint) || Blueprint->IsUpToDate())
		{
			continue;
		}
		if (!FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint) && Blueprint->IsPossiblyDirty() && Blueprint->Status != BS_Unknown)
		{
			ToCompile.Add(Blueprint);
		}
		else if (Blueprint->Status == BS_Error && Blueprint->bDisplayCompilePIEWarning)
		{
			Errored.Add(Blueprint);
		}
	}
	TArray<FBlueprintErrors> Result;
	for (UBlueprint* Blueprint : ToCompile)
	{
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		if (Blueprint->Status == BS_Error)
		{
			FBlueprintErrors& Entry = Result.AddDefaulted_GetRef();
			Entry.Blueprint = Blueprint;
			CollectMessages(Log, Entry.Messages);
		}
	}
	for (UBlueprint* Blueprint : Errored)
	{
		// Compile again only to read the errors (the editor keeps none); it stays in error.
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		if (Blueprint->Status == BS_Error)
		{
			FBlueprintErrors& Entry = Result.AddDefaulted_GetRef();
			Entry.Blueprint = Blueprint;
			CollectMessages(Log, Entry.Messages);
		}
	}
	return Result;
}

const TCHAR* const StartFields[] = {TEXT("action"), TEXT("mode"), TEXT("map"), TEXT("save_first"), TEXT("allow_blueprint_errors"),
	TEXT("paused"), TEXT("console"), TEXT("timeout")};
const TCHAR* const WaitFields[] = {TEXT("action"), TEXT("seconds"), TEXT("until"), TEXT("timeout")};
const TCHAR* const InputFields[] = {TEXT("action"), TEXT("inputs"), TEXT("settle"), TEXT("timeout")};
const TCHAR* const InputEntryFields[] = {TEXT("input_action"), TEXT("key"), TEXT("value"), TEXT("duration"), TEXT("at"), TEXT("state")};

// The first word of a console command that acts on the editor rather than the game, or empty.
FString RefusedConsoleWord(const FString& Command)
{
	static const TCHAR* const Refused[] = {TEXT("quit"), TEXT("exit"), TEXT("crash"), TEXT("debug"), TEXT("forcegc"), TEXT("obj"), TEXT("py"),
		TEXT("exec")};
	TArray<FString> Lines;
	Command.ParseIntoArray(Lines, TEXT("|"));
	for (const FString& Line : Lines)
	{
		FString First;
		const FString Trimmed = Line.TrimStartAndEnd();
		if (!Trimmed.Split(TEXT(" "), &First, nullptr))
		{
			First = Trimmed;
		}
		for (const TCHAR* Word : Refused)
		{
			if (First.Equals(Word, ESearchCase::IgnoreCase))
			{
				return First;
			}
		}
	}
	return FString();
}

template<int32 N>
bool CheckFields(const FJsonObject& Json, const TCHAR* const (&Known)[N], const FString& Prefix, const FString& What,
	TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	for (const auto& Field : Json.Values)
	{
		bool bKnown = false;
		TArray<FString> Names;
		for (const TCHAR* Name : Known)
		{
			bKnown |= Field.Key == Name;
			Names.Add(Name);
		}
		if (!bKnown)
		{
			Diagnostics.Add(PlayInvalidRequest(Prefix + TEXT("/") + Field.Key, FString::Printf(TEXT("Unknown %s field '%s'. Fields: %s."), *What,
				*Field.Key, *FString::Join(Names, TEXT(", ")))));
		}
	}
	return Diagnostics.IsEmpty();
}

bool ReadTimeout(const FJsonObject& Json, double Default, double& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	Out = Default;
	if (Json.HasField(TEXT("timeout")) && (!Json.TryGetNumberField(TEXT("timeout"), Out) || Out <= 0.0 || Out > MaximumTimeout))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/timeout"), FString::Printf(TEXT("'timeout' is real seconds, more than 0 and at most %g."), MaximumTimeout)));
		return false;
	}
	return true;
}

FLevelToolResult FPlayManager::Start(const FJsonObject& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	CheckFields(Request, StartFields, FString(), TEXT("play start"), Diagnostics);
	FString Mode = TEXT("window");
	Request.TryGetStringField(TEXT("mode"), Mode);
	if (Mode != TEXT("window") && Mode != TEXT("viewport") && Mode != TEXT("simulate"))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/mode"), TEXT("'mode' is \"window\" (default: a new 1280x720 PIE window), \"viewport\" (the active Level Editor viewport) or \"simulate\".")));
	}
	bool bAllowErrors = false;
	Request.TryGetBoolField(TEXT("allow_blueprint_errors"), bAllowErrors);
	// console: commands typed before the first frame (after BeginPlay), e.g. a cheat or "ke Boss SetAutoAttack false".
	TArray<FString> StartCommands;
	if (const TSharedPtr<FJsonValue> ConsoleField = Request.TryGetField(TEXT("console")))
	{
		TArray<TSharedPtr<FJsonValue>> Entries;
		if (ConsoleField->Type == EJson::Array)
		{
			Entries = ConsoleField->AsArray();
		}
		else
		{
			Entries.Add(ConsoleField);
		}
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			FString Command;
			const FString Path = ConsoleField->Type == EJson::Array ? FString::Printf(TEXT("/console/%d"), Index) : FString(TEXT("/console"));
			if (!Entries[Index].IsValid() || !Entries[Index]->TryGetString(Command) || Command.TrimStartAndEnd().IsEmpty())
			{
				Diagnostics.Add(PlayInvalidRequest(Path, TEXT("'console' is a command or an array of commands, run in order before the first frame.")));
				continue;
			}
			const FString Refused = RefusedConsoleWord(Command);
			if (!Refused.IsEmpty())
			{
				Diagnostics.Add(MakePlayDiagnostic(Path, TEXT("Play.CommandRefused"), FString::Printf(
					TEXT("'%s' is refused here: it would affect the editor, not only the game."), *Refused)));
				continue;
			}
			StartCommands.Add(Command.TrimStartAndEnd());
		}
		if (Mode == TEXT("simulate") && !StartCommands.IsEmpty())
		{
			Diagnostics.Add(PlayInvalidRequest(TEXT("/console"), TEXT("Simulate has no player to type console commands: use \"mode\": \"window\" or \"viewport\".")));
		}
	}
	double Timeout = StartTimeout;
	ReadTimeout(Request, StartTimeout, Timeout, Diagnostics);
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (GEditor == nullptr || GUnrealEd == nullptr || GEditor->GetEditorWorldContext().World() == nullptr)
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.EditorBusy"), TEXT("No editor world is open; retry shortly.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress())
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.AlreadyRunning"),
			TEXT("A play session is already running (see payload.session); stop it first or keep using it.")));
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetObjectField(TEXT("session"), Status());
		return FLevelToolResult::Failure(MoveTemp(Diagnostics), Payload);
	}
	if (GEditor->IsTransactionActive()
		|| (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()))
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.EditorBusy"),
			TEXT("An editor operation or modal dialog is in progress (starting Play would cancel it); retry when it ends.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	FString Map;
	TArray<FAssetDocumentDiagnostic> Warnings;
	if (Request.TryGetStringField(TEXT("map"), Map) && !Map.IsEmpty())
	{
		TSharedRef<FJsonObject> OpenRequest = MakeShared<FJsonObject>();
		OpenRequest->SetStringField(TEXT("path"), Map);
		bool bSaveFirst = false;
		if (Request.TryGetBoolField(TEXT("save_first"), bSaveFirst))
		{
			OpenRequest->SetBoolField(TEXT("save_first"), bSaveFirst);
		}
		FLevelToolResult Opened = RunOpenLevel(OpenRequest);
		if (!Opened.bSuccess)
		{
			return Opened;
		}
	}

	const TArray<FBlueprintErrors> Errors = CompileForPlay();
	if (!Errors.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> List;
		TArray<FString> Names;
		for (const FBlueprintErrors& Entry : Errors)
		{
			UBlueprint* Blueprint = Entry.Blueprint.Get();
			if (Blueprint == nullptr)
			{
				continue;
			}
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
			Item->SetArrayField(TEXT("errors"), PlayStringArray(Entry.Messages));
			List.Add(MakeShared<FJsonValueObject>(Item));
			Names.Add(Blueprint->GetName());
			if (bAllowErrors)
			{
				// What the editor's "Play anyway" does: do not warn again until it compiles again.
				Blueprint->bDisplayCompilePIEWarning = false;
			}
		}
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("blueprints"), List);
		if (!bAllowErrors)
		{
			Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.BlueprintCompileErrors"), FString::Printf(
				TEXT("%d Blueprint(s) have compile errors: %s (details.blueprints lists the errors). Fix them, or pass "
					"\"allow_blueprint_errors\": true to play anyway as the editor's prompt allows."), Names.Num(), *FString::Join(Names, TEXT(", "))),
				Details));
			return FLevelToolResult::Failure(MoveTemp(Diagnostics));
		}
		Warnings.Add(MakeWarning(TEXT(""), TEXT("Play.BlueprintCompileErrors"),
			FString::Printf(TEXT("Playing with Blueprint compile errors: %s."), *FString::Join(Names, TEXT(", "))), Details));
	}

	FRequestPlaySessionParams Params;
	ULevelEditorPlaySettings* Settings = DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
	Settings->SetPlayNumberOfClients(1);
	Settings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
	Settings->SetRunUnderOneProcess(true);
	Settings->bLaunchSeparateServer = false;
	Settings->NewWindowWidth = 1280;
	Settings->NewWindowHeight = 720;
	Params.EditorPlaySettings = Settings;
	const bool bSimulate = Mode == TEXT("simulate");
	if (bSimulate)
	{
		Params.WorldType = EPlaySessionWorldType::SimulateInEditor;
	}
	if (Mode != TEXT("window"))
	{
		FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		TSharedPtr<IAssetViewport> Viewport = LevelEditor.GetFirstActiveViewport();
		if (!Viewport.IsValid())
		{
			Diagnostics.Add(MakePlayDiagnostic(TEXT("/mode"), TEXT("Play.NoViewport"),
				FString::Printf(TEXT("No Level Editor viewport to %s in; use \"mode\": \"window\"."), bSimulate ? TEXT("simulate") : TEXT("play"))));
			return FLevelToolResult::Failure(MoveTemp(Diagnostics));
		}
		Params.DestinationSlateViewport = Viewport;
	}
	if (!bSimulate && GEditor->CheckForPlayerStart() == nullptr)
	{
		if (FLevelEditorViewportClient* Client = FAssetDocumentViewportCapture::FindViewportClient())
		{
			Params.StartLocation = Client->GetViewLocation();
			Params.StartRotation = FRotator(0.0, Client->GetViewRotation().Yaw, 0.0);
		}
		Warnings.Add(MakeWarning(TEXT(""), TEXT("Play.NoPlayerStart"),
			TEXT("The map has no Player Start: the player starts at the editor camera, as the editor does (unless a placed pawn auto-possesses).")));
	}

	TSharedPtr<FOperation> Operation = AddOperation(EOperationKind::Start, Timeout);
	Operation->Mode = Mode;
	Operation->bSimulate = bSimulate;
	Request.TryGetBoolField(TEXT("paused"), Operation->bStartPaused);
	Operation->StartCommands = MoveTemp(StartCommands);
	Operation->Warnings = Warnings;
	SessionMode = Mode;
	bToolSession = true;
	GUnrealEd->RequestPlaySession(Params);
	return PendingResult(*Operation, TEXT("Play requested; the session starts on the next editor frame."));
}

void FPlayManager::UpdateStart(FOperation& Operation)
{
	const double Now = FPlatformTime::Seconds();
	UWorld* World = GetPlayWorld();
	if (World != nullptr && World->HasBegunPlay() && !GEditor->IsPlaySessionRequestQueued())
	{
		if (Operation.BegunReal == 0.0)
		{
			Operation.BegunReal = Now;
		}
		APlayerController* Controller = GetLocalPlayerController(*World);
		const bool bPlayerReady = Operation.bSimulate || (Controller != nullptr && Controller->GetPawn() != nullptr);
		// A paused start ends at world time 0: paused before the world's first tick.
		if ((bPlayerReady && (World->GetTimeSeconds() > 0.0 || Operation.bPausedAtStart)) || Now - Operation.BegunReal > 2.0)
		{
			if (!bPlayerReady)
			{
				Operation.Warnings.Add(MakeWarning(TEXT(""), TEXT("Play.NoPlayerPawn"),
					TEXT("The play session runs but the player controls no pawn (check the GameMode's Default Pawn Class / auto possess).")));
			}
			if (Operation.bStartPaused)
			{
				GEditor->SetPIEWorldsPaused(true);
			}
			UEnhancedInputLocalPlayerSubsystem* InputSubsystem = Controller != nullptr ? GetInputSubsystem(*Controller) : nullptr;
			const TArray<FString> Remapped = InputSubsystem != nullptr ? DescribeCustomizedPlayerKeys(*InputSubsystem) : TArray<FString>();
			if (!Remapped.IsEmpty())
			{
				// Saved key settings (EnhancedInputUserSettings) load in every session and override the assets' keys.
				Operation.Warnings.Add(MakeWarning(TEXT(""), TEXT("Play.InputUserSettingsOverride"), FString::Printf(
					TEXT("The player's saved key settings remap %s: those keys, not the assets', work in this session. query_play "
						"{\"what\": \"input\"} shows them; play {\"action\": \"reset_input_settings\"} resets them."),
					*FString::Join(Remapped, TEXT(", ")))));
			}
			TSharedRef<FJsonObject> Payload = ResultPayload(Operation);
			if (!Operation.StartConsole.IsEmpty())
			{
				Payload->SetArrayField(TEXT("console"), Operation.StartConsole);
			}
			Finish(Operation, FLevelToolResult::Success(FString::Printf(TEXT("%s %s (%s)."), Operation.bStartPaused ? TEXT("Paused at the start of")
				: TEXT("Playing"), *World->GetOutermost()->GetName(), *Operation.Mode), Payload));
		}
		return;
	}
	if (World == nullptr && !GEditor->IsPlaySessionInProgress())
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.StartFailed"),
			TEXT("The editor did not start the play session; payload.log lists the errors it logged.")));
		bToolSession = false;
		Finish(Operation, FLevelToolResult::Failure(MoveTemp(Diagnostics), ResultPayload(Operation)));
		return;
	}
	if (Now > Operation.DeadlineReal)
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.Timeout"),
			TEXT("The play session did not finish starting in time (payload.session shows its state).")));
		Finish(Operation, FLevelToolResult::Failure(MoveTemp(Diagnostics), ResultPayload(Operation)));
	}
}

FLevelToolResult FPlayManager::Stop(const FJsonObject& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	const TCHAR* const Fields[] = {TEXT("action")};
	if (!CheckFields(Request, Fields, FString(), TEXT("play stop"), Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (GEditor == nullptr || (GEditor->PlayWorld == nullptr && !GEditor->IsPlaySessionInProgress()))
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetObjectField(TEXT("session"), Status());
		return FLevelToolResult::Success(TEXT("No play session is running."), Payload);
	}
	UWorld* World = GetPlayWorld();
	if (bInputFrozen)
	{
		ThawHeldInputs();
	}
	ReleaseAllInputs(World != nullptr ? GetLocalPlayerController(*World) : nullptr);
	ReleaseAllHeld(World != nullptr ? GetLocalPlayerController(*World) : nullptr);
	TSharedPtr<FOperation> Operation = AddOperation(EOperationKind::Stop, StopTimeout);
	Operation->LogSeq = SessionLogSeq;
	Operation->SessionRealSeconds = FPlatformTime::Seconds() - SessionStartReal;
	Operation->LastWorldTime = World != nullptr ? World->GetTimeSeconds() : 0.0;
	GEditor->RequestEndPlayMap();
	return PendingResult(*Operation, TEXT("Stop requested."));
}

void FPlayManager::UpdateStop(FOperation& Operation)
{
	if (GEditor->PlayWorld == nullptr && !GEditor->IsPlaySessionInProgress())
	{
		TSharedRef<FJsonObject> Payload = ResultPayload(Operation);
		Payload->SetNumberField(TEXT("played_real_seconds"), Round3(Operation.SessionRealSeconds));
		Payload->SetNumberField(TEXT("played_unpaused_real_seconds"), Round3(UnpausedReal));
		Payload->SetNumberField(TEXT("final_world_time"), Round3(Operation.LastWorldTime));
		Finish(Operation, FLevelToolResult::Success(TEXT("Play session stopped; payload.log summarizes the session's errors and warnings."), Payload));
	}
	else if (FPlatformTime::Seconds() > Operation.DeadlineReal)
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.Timeout"), TEXT("The play session has not ended yet.")));
		Finish(Operation, FLevelToolResult::Failure(MoveTemp(Diagnostics), ResultPayload(Operation)));
	}
}

FLevelToolResult NoSession()
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.NoSession"),
		TEXT("No Play In Editor session is running: start one with {\"action\": \"start\"}.")));
	return FLevelToolResult::Failure(MoveTemp(Diagnostics));
}

FLevelToolResult FPlayManager::Pause(bool bPause)
{
	UWorld* World = GetPlayWorld();
	if (World == nullptr)
	{
		return NoSession();
	}
	GEditor->SetPIEWorldsPaused(bPause);
	World->bDebugFrameStepExecution = false;
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetObjectField(TEXT("session"), Status());
	return FLevelToolResult::Success(bPause ? TEXT("Paused.") : TEXT("Resumed."), Payload);
}

// ---------------------------------------------------------------------------------------------
// wait

FLevelToolResult FPlayManager::Wait(const FJsonObject& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	CheckFields(Request, WaitFields, FString(), TEXT("play wait"), Diagnostics);
	UWorld* World = GetPlayWorld();
	double Seconds = 0.0;
	const bool bSeconds = Request.TryGetNumberField(TEXT("seconds"), Seconds);
	if (Request.HasField(TEXT("seconds")) && (!bSeconds || Seconds <= 0.0 || Seconds > 600.0))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/seconds"), TEXT("'seconds' is game time, more than 0 and at most 600.")));
	}
	TArray<FPlayCondition> Conditions;
	if (Request.HasField(TEXT("until")))
	{
		const TSharedPtr<FJsonValue> Until = Request.TryGetField(TEXT("until"));
		TArray<TSharedPtr<FJsonValue>> Entries;
		if (Until->Type == EJson::Array)
		{
			Entries = Until->AsArray();
		}
		else
		{
			Entries.Add(Until);
		}
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			FPlayCondition Condition;
			if (ParseCondition(World, Entries[Index], Until->Type == EJson::Array ? FString::Printf(TEXT("/until/%d"), Index) : FString(TEXT("/until")),
				Condition, Diagnostics))
			{
				Conditions.Add(MoveTemp(Condition));
			}
		}
	}
	if (!bSeconds && Conditions.IsEmpty() && Diagnostics.IsEmpty())
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT(""), TEXT("wait needs 'seconds' (game time to advance) and/or 'until' (conditions; any one ends the wait).")));
	}
	double Timeout = DefaultWaitTimeout;
	ReadTimeout(Request, FMath::Min(MaximumTimeout, bSeconds ? DefaultWaitTimeout + Seconds * 2.0 : DefaultWaitTimeout), Timeout, Diagnostics);
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	const bool bOnlySessionEnd = Conditions.Num() > 0 && Conditions.ContainsByPredicate([](const FPlayCondition& Condition)
	{
		return Condition.Kind == FPlayCondition::EKind::SessionEnded;
	});
	if (World == nullptr && !bOnlySessionEnd)
	{
		return NoSession();
	}
	TSharedPtr<FOperation> Operation = AddOperation(EOperationKind::Wait, Timeout);
	Operation->Conditions = MoveTemp(Conditions);
	if (World != nullptr)
	{
		Operation->StartWorld = World->GetTimeSeconds();
		if (bSeconds)
		{
			Operation->UntilWorld = Operation->StartWorld + Seconds;
		}
		if (World->bDebugPauseExecution)
		{
			GEditor->SetPIEWorldsPaused(false);
			Operation->bRepause = true;
		}
	}
	UpdateWait(*Operation, World, false);
	if (Operation->bDone)
	{
		Operations.Remove(Operation->Id);
		return Operation->Result;
	}
	return PendingResult(*Operation, TEXT("Waiting."));
}

void FPlayManager::CompleteWait(FOperation& Operation, UWorld* World, const FString& Reason, int32 Condition, TSharedPtr<FJsonValue> Observed)
{
	if (Operation.bRepause && World != nullptr && GEditor->PlayWorld != nullptr)
	{
		GEditor->SetPIEWorldsPaused(true);
	}
	TSharedRef<FJsonObject> Payload = ResultPayload(Operation);
	const bool bSatisfied = Reason == TEXT("condition") || (Reason == TEXT("time") && Operation.Conditions.IsEmpty())
		|| (Reason == TEXT("session_ended") && Condition >= 0);
	Payload->SetBoolField(TEXT("satisfied"), bSatisfied);
	Payload->SetStringField(TEXT("reason"), Reason);
	if (Condition >= 0)
	{
		Payload->SetNumberField(TEXT("condition"), Condition);
		Payload->SetStringField(TEXT("condition_text"), Operation.Conditions[Condition].Text);
	}
	if (Observed.IsValid())
	{
		Payload->SetField(TEXT("observed"), Observed);
	}
	if (World != nullptr && GEditor->PlayWorld == World)
	{
		Payload->SetNumberField(TEXT("waited_game_seconds"), Round3(World->GetTimeSeconds() - Operation.StartWorld));
	}
	Payload->SetNumberField(TEXT("waited_real_seconds"), Round3(FPlatformTime::Seconds() - Operation.StartReal));
	FLevelToolResult Result = FLevelToolResult::Success(FString(), Payload);
	if (bSatisfied)
	{
		Result.Message = Condition >= 0 ? FString::Printf(TEXT("Condition met: %s."), *Operation.Conditions[Condition].Text)
			: FString::Printf(TEXT("Advanced %.2f game seconds."), World != nullptr ? World->GetTimeSeconds() - Operation.StartWorld : 0.0);
	}
	else
	{
		TArray<FString> Texts;
		TArray<TSharedPtr<FJsonValue>> Last;
		for (const FPlayCondition& Entry : Operation.Conditions)
		{
			Texts.Add(Entry.Text);
			TSharedPtr<FJsonValue> Value;
			EvaluateCondition(World != nullptr && GEditor->PlayWorld == World ? World : nullptr, Entry, Operation.LogSeq, Value);
			Last.Add(Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		}
		Payload->SetArrayField(TEXT("last_observed"), Last);
		Result.Message = FString::Printf(TEXT("Not met (%s): %s."), *Reason, *FString::Join(Texts, TEXT(" | ")));
		Result.Diagnostics.Add(MakeWarning(TEXT("/until"), Reason == TEXT("session_ended") ? TEXT("Play.SessionEnded") : TEXT("Play.WaitTimedOut"),
			Reason == TEXT("session_ended") ? TEXT("The play session ended before the condition was met.")
				: TEXT("The wait ended before any condition was met; payload.last_observed has the current values.")));
	}
	Finish(Operation, MoveTemp(Result));
}

void FPlayManager::UpdateWait(FOperation& Operation, UWorld* World, bool bPostTick)
{
	if (World == nullptr || GEditor->PlayWorld == nullptr)
	{
		const int32 SessionEnd = Operation.Conditions.IndexOfByPredicate([](const FPlayCondition& Condition)
		{
			return Condition.Kind == FPlayCondition::EKind::SessionEnded;
		});
		CompleteWait(Operation, nullptr, TEXT("session_ended"), SessionEnd, nullptr);
		return;
	}
	for (int32 Index = 0; Index < Operation.Conditions.Num(); ++Index)
	{
		TSharedPtr<FJsonValue> Observed;
		if (EvaluateCondition(World, Operation.Conditions[Index], Operation.LogSeq, Observed))
		{
			CompleteWait(Operation, World, TEXT("condition"), Index, Observed);
			return;
		}
	}
	if (Operation.UntilWorld.IsSet() && World->GetTimeSeconds() >= Operation.UntilWorld.GetValue())
	{
		CompleteWait(Operation, World, TEXT("time"), INDEX_NONE, nullptr);
		return;
	}
	if (FPlatformTime::Seconds() > Operation.DeadlineReal)
	{
		CompleteWait(Operation, World, TEXT("timeout"), INDEX_NONE, nullptr);
	}
}

// ---------------------------------------------------------------------------------------------
// input

const UInputAction* ResolveInputAction(const FString& Name, APlayerController* Controller, FString& OutCode, FString& OutMessage)
{
	if (Name.StartsWith(TEXT("/")))
	{
		FString ObjectPath = Name;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath += TEXT(".") + FPackageName::GetShortName(ObjectPath);
		}
		if (const UInputAction* Action = LoadObject<UInputAction>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Action;
		}
		OutCode = TEXT("Play.InputActionNotFound");
		OutMessage = FString::Printf(TEXT("No Input Action at '%s'."), *Name);
		return nullptr;
	}
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, true);
	TArray<FAssetData> Matches;
	TArray<FString> Close;
	for (const FAssetData& Asset : Assets)
	{
		const FString AssetName = Asset.AssetName.ToString();
		if (AssetName.Equals(Name, ESearchCase::IgnoreCase))
		{
			Matches.Add(Asset);
		}
		else if (Close.Num() < 8 && (AssetName.Contains(Name, ESearchCase::IgnoreCase) || Name.Contains(AssetName, ESearchCase::IgnoreCase)))
		{
			Close.Add(Asset.GetObjectPathString());
		}
	}
	if (Matches.Num() > 1 && Controller != nullptr)
	{
		// Prefer the one the player's mapping contexts map.
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem(*Controller))
		{
			TArray<FAssetData> Mapped = Matches.FilterByPredicate([Subsystem](const FAssetData& Asset)
			{
				const UInputAction* Action = Cast<UInputAction>(Asset.FastGetAsset(false));
				return Action != nullptr && !Subsystem->QueryKeysMappedToAction(Action).IsEmpty();
			});
			if (Mapped.Num() == 1)
			{
				Matches = Mapped;
			}
		}
	}
	if (Matches.Num() == 1)
	{
		return Cast<UInputAction>(Matches[0].GetAsset());
	}
	if (Matches.Num() > 1)
	{
		TArray<FString> Paths;
		for (const FAssetData& Asset : Matches)
		{
			Paths.Add(Asset.GetObjectPathString());
		}
		OutCode = TEXT("Play.InputActionAmbiguous");
		OutMessage = FString::Printf(TEXT("Several Input Actions are named '%s': %s. Use the path."), *Name, *FString::Join(Paths, TEXT(", ")));
		return nullptr;
	}
	OutCode = TEXT("Play.InputActionNotFound");
	OutMessage = FString::Printf(TEXT("No Input Action named '%s'.%s"), *Name,
		Close.IsEmpty() ? TEXT(" find_assets with class /Script/EnhancedInput.InputAction lists them.") : *FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", "))));
	return nullptr;
}

bool ReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
	if (!Value.IsValid())
	{
		return false;
	}
	double Number = 0.0;
	bool bBool = false;
	if (Value->TryGetNumber(Number))
	{
		Out = FVector(Number, 0.0, 0.0);
		return true;
	}
	if (Value->TryGetBool(bBool))
	{
		Out = FVector(bBool ? 1.0 : 0.0, 0.0, 0.0);
		return true;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Value->TryGetObject(Object) && Object != nullptr)
	{
		Out = FVector::ZeroVector;
		(*Object)->TryGetNumberField(TEXT("X"), Out.X);
		(*Object)->TryGetNumberField(TEXT("Y"), Out.Y);
		(*Object)->TryGetNumberField(TEXT("Z"), Out.Z);
		return true;
	}
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Value->TryGetArray(Array) && Array != nullptr && Array->Num() <= 3)
	{
		Out = FVector::ZeroVector;
		for (int32 Index = 0; Index < Array->Num(); ++Index)
		{
			double Component = 0.0;
			if (!(*Array)[Index]->TryGetNumber(Component))
			{
				return false;
			}
			Out[Index] = Component;
		}
		return true;
	}
	return false;
}

bool ResolveKey(const FString& Name, FKey& Out, TArray<FString>& OutClose)
{
	Out = FKey(FName(*Name));
	if (Out.IsValid())
	{
		return true;
	}
	TArray<FKey> AllKeys;
	EKeys::GetAllKeys(AllKeys);
	for (const FKey& Key : AllKeys)
	{
		const FString KeyName = Key.GetFName().ToString();
		if (KeyName.Equals(Name, ESearchCase::IgnoreCase) || Key.GetDisplayName(false).ToString().Equals(Name, ESearchCase::IgnoreCase))
		{
			Out = Key;
			return true;
		}
		if (OutClose.Num() < 8 && KeyName.Contains(Name, ESearchCase::IgnoreCase))
		{
			OutClose.Add(KeyName);
		}
	}
	return false;
}

FLevelToolResult FPlayManager::HandleInput(const FJsonObject& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	CheckFields(Request, InputFields, FString(), TEXT("play input"), Diagnostics);
	UWorld* World = GetPlayWorld();
	if (World == nullptr)
	{
		return NoSession();
	}
	APlayerController* Controller = GetLocalPlayerController(*World);
	UEnhancedInputLocalPlayerSubsystem* Subsystem = Controller != nullptr ? GetInputSubsystem(*Controller) : nullptr;
	if (Controller == nullptr || Subsystem == nullptr)
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.NoPlayer"), Controller == nullptr
			? TEXT("The play session has no local player (Simulate has none): input needs \"mode\": \"window\" or \"viewport\".")
			: TEXT("The local player has no Enhanced Input subsystem.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Request.TryGetArrayField(TEXT("inputs"), Entries) || Entries == nullptr || Entries->IsEmpty() || Entries->Num() > MaxInputs)
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/inputs"), FString::Printf(TEXT("'inputs' is an array of 1 to %d inputs: {\"input_action\" | \"key\", "
			"\"value\"?, \"duration\"? (game s; 0 = one frame), \"at\"? (game s after the request), \"state\"? (\"down\": keep it held, \"up\": release it)}."),
			MaxInputs)));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	double Settle = 0.0;
	if (Request.HasField(TEXT("settle")) && (!Request.TryGetNumberField(TEXT("settle"), Settle) || Settle < 0.0 || Settle > MaxInputSeconds))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/settle"), TEXT("'settle' is game seconds to keep playing after the last input ends (0 to 60).")));
	}
	TArray<FScheduledInput> Inputs;
	TArray<FAssetDocumentDiagnostic> Warnings;
	double LastEnd = 0.0;
	// Held after this request's downs and ups, in request order: an up must name an input held by then.
	TSet<FString> HeldAfter;
	for (const TPair<FString, FHeldInput>& Held : HeldInputs)
	{
		HeldAfter.Add(Held.Key);
	}
	for (int32 Index = 0; Index < Entries->Num(); ++Index)
	{
		const FString Path = FString::Printf(TEXT("/inputs/%d"), Index);
		const TSharedPtr<FJsonObject> Entry = (*Entries)[Index].IsValid() && (*Entries)[Index]->Type == EJson::Object ? (*Entries)[Index]->AsObject() : nullptr;
		if (!Entry.IsValid())
		{
			Diagnostics.Add(PlayInvalidRequest(Path, TEXT("An input is an object.")));
			continue;
		}
		CheckFields(*Entry, InputEntryFields, Path, TEXT("input"), Diagnostics);
		FScheduledInput Input;
		Input.Index = Index;
		if (Entry->HasField(TEXT("at")) && (!Entry->TryGetNumberField(TEXT("at"), Input.At) || Input.At < 0.0 || Input.At > MaxInputSeconds))
		{
			Diagnostics.Add(PlayInvalidRequest(Path + TEXT("/at"), TEXT("'at' is game seconds after the request (0 to 60).")));
		}
		if (Entry->HasField(TEXT("duration")) && (!Entry->TryGetNumberField(TEXT("duration"), Input.Duration) || Input.Duration < 0.0 || Input.Duration > MaxInputSeconds))
		{
			Diagnostics.Add(PlayInvalidRequest(Path + TEXT("/duration"), TEXT("'duration' is game seconds to hold (0 = one frame, a tap; at most 60).")));
		}
		FString State;
		if (Entry->HasField(TEXT("state")))
		{
			Entry->TryGetStringField(TEXT("state"), State);
			if (State == TEXT("down"))
			{
				Input.Mode = FScheduledInput::EMode::Down;
			}
			else if (State == TEXT("up"))
			{
				Input.Mode = FScheduledInput::EMode::Up;
				if (Entry->HasField(TEXT("duration")) || Entry->HasField(TEXT("value")))
				{
					Diagnostics.Add(PlayInvalidRequest(Path, TEXT("An input with \"state\": \"up\" only releases a held input: no 'duration' or 'value'.")));
				}
				Input.Duration = 0.0;
			}
			else if (State != TEXT("tap"))
			{
				Diagnostics.Add(PlayInvalidRequest(Path + TEXT("/state"), TEXT("'state' is \"down\" (press and keep it held after this request, for hold or "
					"charge moves), \"up\" (release an input a \"down\" left held) or \"tap\" (default: press, hold 'duration', release).")));
			}
		}
		LastEnd = FMath::Max(LastEnd, Input.At + Input.Duration);
		FString ActionName;
		FString KeyName;
		const bool bAction = Entry->TryGetStringField(TEXT("input_action"), ActionName);
		const bool bKey = Entry->TryGetStringField(TEXT("key"), KeyName);
		if (bAction == bKey)
		{
			Diagnostics.Add(PlayInvalidRequest(Path, TEXT("An input names exactly one of 'input_action' (an Input Action path or name) or 'key' (a key name such as SpaceBar, LeftMouseButton, Gamepad_LeftX).")));
			continue;
		}
		const TSharedPtr<FJsonValue> Value = Entry->TryGetField(TEXT("value"));
		if (bAction)
		{
			FString Code;
			FString Message;
			const UInputAction* Action = ResolveInputAction(ActionName, Controller, Code, Message);
			if (Action == nullptr)
			{
				Diagnostics.Add(MakePlayDiagnostic(Path + TEXT("/input_action"), Code, Message));
				continue;
			}
			FVector Vector(1.0, 0.0, 0.0);
			if (Value.IsValid() && !ReadVectorValue(Value, Vector))
			{
				Diagnostics.Add(PlayInvalidRequest(Path + TEXT("/value"), TEXT("'value' is true/false, a number, or {\"X\", \"Y\", \"Z\"} for 2D/3D actions.")));
				continue;
			}
			if (Value.IsValid())
			{
				const FString Expected = PlaySessionLocal::ActionValueMismatch(*Action, *Value, Vector);
				if (!Expected.IsEmpty())
				{
					Diagnostics.Add(MakePlayDiagnostic(Path + TEXT("/value"), TEXT("Play.InputValueTypeMismatch"), FString::Printf(
						TEXT("%s is a %s action (its ValueType): 'value' is %s. This value does not fit it (the action would get %s)."),
						*Action->GetName(), *PlaySessionLocal::ValueTypeName(Action->ValueType), *Expected,
						*FInputActionValue(Action->ValueType, Vector).ToString())));
					continue;
				}
			}
			if (!Value.IsValid() && Input.Mode != FScheduledInput::EMode::Up && Action->ValueType != EInputActionValueType::Boolean
				&& Action->ValueType != EInputActionValueType::Axis1D)
			{
				Diagnostics.Add(PlayInvalidRequest(Path + TEXT("/value"), FString::Printf(TEXT("%s is a %s action: give 'value' {\"X\", \"Y\"%s}."),
					*Action->GetName(), Action->ValueType == EInputActionValueType::Axis2D ? TEXT("2D") : TEXT("3D"),
					Action->ValueType == EInputActionValueType::Axis3D ? TEXT(", \"Z\"") : TEXT(""))));
				continue;
			}
			Input.Action.Reset(const_cast<UInputAction*>(Action));
			Input.ActionValue = FInputActionValue(Action->ValueType, Vector);
			FScheduledInput::FWatched& Watched = Input.Watched.AddDefaulted_GetRef();
			Watched.Action = Action;
			for (const FKey& Key : Subsystem->QueryKeysMappedToAction(Action))
			{
				Input.MappedKeys.AddUnique(Key.GetFName().ToString());
			}
			if (Input.MappedKeys.IsEmpty())
			{
				Warnings.Add(MakeWarning(Path + TEXT("/input_action"), TEXT("Play.ActionNotMapped"), FString::Printf(
					TEXT("No mapping context the player has added maps %s: a player could not trigger it with a key (injected anyway)."), *Action->GetName())));
			}
		}
		else
		{
			TArray<FString> Close;
			if (!ResolveKey(KeyName, Input.Key, Close))
			{
				Diagnostics.Add(MakePlayDiagnostic(Path + TEXT("/key"), TEXT("Play.UnknownKey"), FString::Printf(TEXT("'%s' is not a key name%s"), *KeyName,
					Close.IsEmpty() ? TEXT(" (e.g. SpaceBar, W, LeftMouseButton, MouseX, Gamepad_LeftX, Gamepad_FaceButton_Bottom).")
						: *FString::Printf(TEXT("; close: %s."), *FString::Join(Close, TEXT(", "))))));
				continue;
			}
			Input.bAnalog = Input.Key.IsAnalog() || EKeys::GetPairedKeyDetails(Input.Key) != nullptr;
			if (Value.IsValid() && !ReadVectorValue(Value, Input.KeyValue))
			{
				Diagnostics.Add(PlayInvalidRequest(Path + TEXT("/value"), TEXT("'value' of an axis key is a number (MouseX: per-frame delta; sticks: -1..1), or {\"X\",\"Y\"} for a 2D key.")));
				continue;
			}
			// The actions this key reaches through the player's live mappings: after context priorities and the
			// player's key settings, and through paired axes (MouseX reaches a Mouse2D mapping).
			if (const UEnhancedPlayerInput* PlayerInput = Subsystem->GetPlayerInput())
			{
				TArray<FLiveKeyReach> Reaches = FindLiveMappingsForKey(*PlayerInput, Input.Key);
				if (Reaches.IsEmpty())
				{
					// A context added since the last rebuild: its mappings go live a frame later.
					Reaches = FindPendingMappingsForKey(*Subsystem, Input.Key);
				}
				for (const FLiveKeyReach& Reach : Reaches)
				{
					FScheduledInput::FWatched& Watched = Input.Watched.AddDefaulted_GetRef();
					Watched.Action = Reach.Action;
					if (Reach.MappingKey != Input.Key)
					{
						Watched.Via = Reach.MappingKey.GetFName().ToString();
					}
				}
			}
			if (Input.Watched.IsEmpty())
			{
				const FString Why = DescribeUnreachedKey(*Subsystem, Input.Key);
				Warnings.Add(MakeWarning(Path + TEXT("/key"), TEXT("Play.KeyNotMapped"), FString::Printf(
					TEXT("No live mapping of the player maps %s to an Input Action (the key is still sent to the player controller)%s. "
						"query_play {\"what\": \"input\"} lists the effective keys."),
					*Input.Key.GetFName().ToString(), Why.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(": %s"), *Why))));
			}
		}
		Input.HeldId = Input.Action.IsValid() ? Input.Action->GetPathName() : Input.Key.GetFName().ToString();
		if (Input.Mode == FScheduledInput::EMode::Up && !HeldAfter.Contains(Input.HeldId))
		{
			TArray<FString> Held = HeldAfter.Array();
			Diagnostics.Add(MakePlayDiagnostic(Path, TEXT("Play.InputNotHeld"), FString::Printf(TEXT("%s is not held (held: %s); \"state\": \"up\" releases "
				"what an earlier \"state\": \"down\" left held."), *Input.HeldId, Held.IsEmpty() ? TEXT("none") : *FString::Join(Held, TEXT(", ")))));
			continue;
		}
		if (Input.Mode == FScheduledInput::EMode::Tap && HeldAfter.Contains(Input.HeldId))
		{
			Diagnostics.Add(MakePlayDiagnostic(Path, TEXT("Play.InputHeld"), FString::Printf(TEXT("%s is held (an earlier \"state\": \"down\"): release it "
				"with \"state\": \"up\" first, or send \"down\" again to change its value."), *Input.HeldId)));
			continue;
		}
		if (Input.Mode == FScheduledInput::EMode::Down)
		{
			HeldAfter.Add(Input.HeldId);
		}
		else if (Input.Mode == FScheduledInput::EMode::Up)
		{
			HeldAfter.Remove(Input.HeldId);
		}
		Inputs.Add(MoveTemp(Input));
	}
	double Timeout = 0.0;
	ReadTimeout(Request, FMath::Min(MaximumTimeout, LastEnd * 2.0 + Settle * 2.0 + 10.0), Timeout, Diagnostics);
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	// Who listens: Enhanced Input event bindings of every input component in the play world.
	for (FScheduledInput& Input : Inputs)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const UEnhancedInputComponent* Component = Cast<UEnhancedInputComponent>(It->InputComponent);
			if (Component == nullptr)
			{
				continue;
			}
			for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Component->GetActionEventBindings())
			{
				if (!Binding.IsValid())
				{
					continue;
				}
				for (const FScheduledInput::FWatched& Watched : Input.Watched)
				{
					if (Binding->GetAction() == Watched.Action.Get())
					{
						const UObject* Owner = Binding->GetUObject();
						const APawn* ListeningPawn = Cast<APawn>(*It);
						Input.Listeners.AddUnique(FString::Printf(TEXT("%s: %s on %s%s"), *Watched.Action->GetName(),
							*TriggerEventName(Binding->GetTriggerEvent()), Owner != nullptr ? *Owner->GetName() : *It->GetName(),
							ListeningPawn != nullptr && !ListeningPawn->InputEnabled() ? TEXT(" (its input is disabled)") : TEXT("")));
					}
				}
			}
		}
	}

	TArray<FString> Blocked;
	if (const APawn* PlayerPawn = Controller->GetPawn(); PlayerPawn != nullptr && !PlayerPawn->InputEnabled())
	{
		Blocked.Add(TEXT("the player's pawn has its input disabled"));
	}
	if (Controller->IsMoveInputIgnored())
	{
		Blocked.Add(TEXT("move input is ignored"));
	}
	if (Controller->IsLookInputIgnored())
	{
		Blocked.Add(TEXT("look input is ignored"));
	}
	if (!Blocked.IsEmpty())
	{
		Warnings.Add(MakeWarning(TEXT(""), TEXT("Play.PlayerInputBlocked"), FString::Printf(
			TEXT("When the input began %s (the game disabled it, e.g. during a cinematic): the pawn may not react."), *FString::Join(Blocked, TEXT(", ")))));
	}
	TSharedPtr<FOperation> Operation = AddOperation(EOperationKind::Input, Timeout);
	Operation->Warnings = Warnings;
	Operation->Settle = Settle;
	Operation->StartWorld = World->GetTimeSeconds();
	for (FScheduledInput& Input : Inputs)
	{
		Input.StartWorld = Operation->StartWorld + Input.At;
		Input.EndWorld = Input.StartWorld + Input.Duration;
	}
	Operation->Inputs = MoveTemp(Inputs);
	Operation->UntilWorld = Operation->StartWorld + LastEnd + Settle;
	if (World->bDebugPauseExecution)
	{
		GEditor->SetPIEWorldsPaused(false);
		Operation->bRepause = true;
	}
	return PendingResult(*Operation, TEXT("Input scheduled."));
}

FInputDeviceId DeviceFor(const APlayerController& Controller)
{
	return IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(Controller.GetPlatformUserId());
}

void SendKey(APlayerController& Controller, const FKey& Key, EInputEvent Event, float Amount)
{
	FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, Event, Amount, Key.IsAnalog() ? 1 : 0, DeviceFor(Controller));
	Controller.InputKey(Args);
}

void SendAxis(APlayerController& Controller, const FKey& Key, const FVector& Value, float DeltaTime)
{
	auto Send = [&Controller, DeltaTime](const FKey& AxisKey, double Amount)
	{
		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(AxisKey, IE_Axis, static_cast<float>(Amount), 1, DeviceFor(Controller));
		Args.DeltaTime = DeltaTime;
		Controller.InputKey(Args);
	};
	if (const EKeys::FPairedKeyDetails* Paired = EKeys::GetPairedKeyDetails(Key))
	{
		Send(Paired->XKeyDetails->GetKey(), Value.X);
		Send(Paired->YKeyDetails->GetKey(), Value.Y);
	}
	else
	{
		Send(Key, Value.X);
	}
}

void FPlayManager::BeginInput(FScheduledInput& Input, APlayerController& Controller, double Now)
{
	Input.State = FScheduledInput::EState::Active;
	Input.ActualStart = Now;
	const FHeldInput* AlreadyHeld = HeldInputs.Find(Input.HeldId);
	if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem = GetInputSubsystem(Controller))
	{
		if (const UEnhancedPlayerInput* PlayerInput = InputSubsystem->GetPlayerInput())
		{
			for (FScheduledInput::FWatched& Watched : Input.Watched)
			{
				const FInputActionInstance* Instance = Watched.Action.IsValid() ? PlayerInput->FindActionInstanceData(Watched.Action.Get()) : nullptr;
				Watched.TriggeredBefore = Instance != nullptr ? Instance->GetLastTriggeredWorldTime() : -1.0f;
			}
		}
	}
	if (Input.Mode == FScheduledInput::EMode::Up)
	{
		// Release what a "down" left held; Completed is observed in the next frames.
		if (AlreadyHeld != nullptr)
		{
			ReleaseHeld(*AlreadyHeld, Controller);
			HeldInputs.Remove(Input.HeldId);
		}
		Input.State = FScheduledInput::EState::Released;
		Input.ActualEnd = Now;
		return;
	}
	if (Input.Action.IsValid())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem(Controller))
		{
			// A second "down" of a held action changes its value.
			Subsystem->StartContinuousInputInjectionForAction(Input.Action.Get(), Input.ActionValue, {}, {});
		}
	}
	else if (!Input.bAnalog && AlreadyHeld == nullptr)
	{
		SendKey(Controller, Input.Key, IE_Pressed, 1.0f);
	}
	if (AlreadyHeld != nullptr)
	{
		// The held entry is replaced when this down ends; until then the scheduled input drives the key.
		HeldInputs.Remove(Input.HeldId);
	}
}

void FPlayManager::ReleaseHeld(const FHeldInput& Held, APlayerController& Controller)
{
	if (Held.Action.IsValid())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem(Controller))
		{
			Subsystem->StopContinuousInputInjectionForAction(Held.Action.Get());
		}
	}
	else if (Held.bAnalog)
	{
		SendAxis(Controller, Held.Key, FVector::ZeroVector, 0.0f);
	}
	else
	{
		SendKey(Controller, Held.Key, IE_Released, 0.0f);
	}
}

void FPlayManager::ReleaseAllHeld(APlayerController* Controller)
{
	if (Controller != nullptr)
	{
		for (const TPair<FString, FHeldInput>& Held : HeldInputs)
		{
			ReleaseHeld(Held.Value, *Controller);
		}
	}
	HeldInputs.Reset();
}

void FPlayManager::UpdateHeldInputFreeze(UWorld& World)
{
	APlayerController* Controller = GetLocalPlayerController(World);
	const bool bFreeze = World.bDebugPauseExecution && !HeldInputs.IsEmpty() && Controller != nullptr;
	if (bInputFrozen && (!bFreeze || FrozenController.Get() != Controller))
	{
		ThawHeldInputs();
	}
	if (bFreeze && !bInputFrozen)
	{
		FreezeHeldInputs(*Controller);
	}
}

void FPlayManager::FreezeHeldInputs(APlayerController& Controller)
{
	// A paused frame ticks the controller only for bTickEvenWhenPaused (APlayerController sets it): without it the
	// controller's player input is not processed while the tool's pause lasts. Keys keep their down state.
	FrozenController = &Controller;
	bFrozenTickEvenWhenPaused = Controller.PrimaryActorTick.bTickEvenWhenPaused;
	Controller.PrimaryActorTick.bTickEvenWhenPaused = false;
	bInputFrozen = true;
	// Held actions are injected again every frame (also paused): stop that, or the injections pile up for the
	// first frame after the pause. The one injected last stays queued, so the action is still held then.
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem(Controller))
	{
		for (const TPair<FString, FHeldInput>& Held : HeldInputs)
		{
			if (Held.Value.Action.IsValid())
			{
				Subsystem->StopContinuousInputInjectionForAction(Held.Value.Action.Get());
			}
		}
	}
}

void FPlayManager::ThawHeldInputs()
{
	bInputFrozen = false;
	APlayerController* Controller = FrozenController.Get();
	FrozenController.Reset();
	if (Controller == nullptr)
	{
		return;
	}
	Controller->PrimaryActorTick.bTickEvenWhenPaused = bFrozenTickEvenWhenPaused;
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem(*Controller))
	{
		for (const TPair<FString, FHeldInput>& Held : HeldInputs)
		{
			if (Held.Value.Action.IsValid())
			{
				Subsystem->StartContinuousInputInjectionForAction(Held.Value.Action.Get(), Held.Value.ActionValue, {}, {});
			}
		}
	}
}

void FPlayManager::ReleaseInput(FScheduledInput& Input, APlayerController* Controller)
{
	if (Input.State != FScheduledInput::EState::Active)
	{
		return;
	}
	Input.State = FScheduledInput::EState::Released;
	if (Controller == nullptr)
	{
		return;
	}
	if (Input.Action.IsValid())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem(*Controller))
		{
			Subsystem->StopContinuousInputInjectionForAction(Input.Action.Get());
		}
	}
	else if (Input.bAnalog)
	{
		SendAxis(*Controller, Input.Key, FVector::ZeroVector, 0.0f);
	}
	else
	{
		SendKey(*Controller, Input.Key, IE_Released, 0.0f);
	}
}

void FPlayManager::ReleaseAllInputs(APlayerController* Controller)
{
	for (TPair<int32, TSharedPtr<FOperation>>& Entry : Operations)
	{
		for (FScheduledInput& Input : Entry.Value->Inputs)
		{
			ReleaseInput(Input, Controller);
		}
	}
}

void FPlayManager::UpdateInput(FOperation& Operation, UWorld* World)
{
	const bool bSessionGone = World == nullptr || GEditor->PlayWorld == nullptr;
	bool bAllDone = true;
	for (const FScheduledInput& Input : Operation.Inputs)
	{
		bAllDone &= Input.State == FScheduledInput::EState::Done;
	}
	const bool bSettled = !bSessionGone && Operation.UntilWorld.IsSet() && World->GetTimeSeconds() >= Operation.UntilWorld.GetValue();
	const bool bTimedOut = FPlatformTime::Seconds() > Operation.DeadlineReal;
	if (!bSessionGone && !(bAllDone && bSettled) && !bTimedOut)
	{
		return;
	}
	if (bTimedOut || bSessionGone)
	{
		ReleaseAllInputs(bSessionGone ? nullptr : GetLocalPlayerController(*World));
	}
	if (Operation.bRepause && !bSessionGone)
	{
		GEditor->SetPIEWorldsPaused(true);
	}
	TArray<TSharedPtr<FJsonValue>> Reports;
	int32 Triggered = 0;
	for (const FScheduledInput& Input : Operation.Inputs)
	{
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetNumberField(TEXT("index"), Input.Index);
		if (Input.Mode != FScheduledInput::EMode::Tap)
		{
			Report->SetStringField(TEXT("state"), Input.Mode == FScheduledInput::EMode::Down ? TEXT("down") : TEXT("up"));
		}
		if (Input.bKeptDown)
		{
			Report->SetBoolField(TEXT("held"), true);
		}
		if (Input.Action.IsValid())
		{
			Report->SetStringField(TEXT("input_action"), Input.Action->GetPathName());
			Report->SetObjectField(TEXT("value"), ActionValueJson(Input.ActionValue));
			Report->SetArrayField(TEXT("mapped_keys"), PlayStringArray(Input.MappedKeys));
		}
		else
		{
			Report->SetStringField(TEXT("key"), Input.Key.GetFName().ToString());
		}
		Report->SetNumberField(TEXT("start"), Round3(Input.ActualStart));
		Report->SetNumberField(TEXT("end"), Round3(Input.ActualEnd));
		Report->SetNumberField(TEXT("held_frames"), Input.ActiveTicks);
		TArray<TSharedPtr<FJsonValue>> Actions;
		bool bAnyTriggered = false;
		for (const FScheduledInput::FWatched& Watched : Input.Watched)
		{
			TSharedRef<FJsonObject> ActionJson = MakeShared<FJsonObject>();
			ActionJson->SetStringField(TEXT("action"), Watched.Action.IsValid() ? Watched.Action->GetName() : FString(TEXT("?")));
			if (!Watched.Via.IsEmpty())
			{
				ActionJson->SetStringField(TEXT("via"), Watched.Via);
			}
			ActionJson->SetBoolField(TEXT("triggered"), Watched.bTriggered);
			ActionJson->SetNumberField(TEXT("triggered_frames"), Watched.TriggeredTicks);
			TArray<FString> Events = Watched.Events.Array();
			Events.Sort();
			ActionJson->SetArrayField(TEXT("events"), PlayStringArray(Events));
			Actions.Add(MakeShared<FJsonValueObject>(ActionJson));
			bAnyTriggered |= Watched.bTriggered;
		}
		Report->SetArrayField(TEXT("actions"), Actions);
		Report->SetArrayField(TEXT("listeners"), PlayStringArray(Input.Listeners));
		if (Input.State != FScheduledInput::EState::Done)
		{
			Report->SetBoolField(TEXT("completed"), false);
		}
		Triggered += bAnyTriggered ? 1 : 0;
		Reports.Add(MakeShared<FJsonValueObject>(Report));
	}
	TSharedRef<FJsonObject> Payload = ResultPayload(Operation);
	Payload->SetArrayField(TEXT("inputs"), Reports);
	if (!bSessionGone)
	{
		Payload->SetNumberField(TEXT("game_seconds"), Round3(World->GetTimeSeconds() - Operation.StartWorld));
	}
	if (bSessionGone || bTimedOut)
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), bSessionGone ? TEXT("Play.SessionEnded") : TEXT("Play.Timeout"),
			bSessionGone ? TEXT("The play session ended before every input was sent.") : TEXT("The inputs did not finish in time (is the game paused or very slow?).")));
		Finish(Operation, FLevelToolResult::Failure(MoveTemp(Diagnostics), Payload));
		return;
	}
	Finish(Operation, FLevelToolResult::Success(FString::Printf(TEXT("%d input(s) sent; %d triggered an Input Action."), Operation.Inputs.Num(), Triggered), Payload));
}

void FPlayManager::OnWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World == nullptr || GEditor == nullptr || World != GEditor->PlayWorld)
	{
		return;
	}
	UpdateHeldInputFreeze(*World);
	if (Operations.IsEmpty() && HeldInputs.IsEmpty())
	{
		return;
	}
	APlayerController* Controller = GetLocalPlayerController(*World);
	if (Controller == nullptr)
	{
		return;
	}
	// Held analog keys send their value every frame, as a stick held in place does.
	if (!World->bDebugPauseExecution)
	{
		for (const TPair<FString, FHeldInput>& Held : HeldInputs)
		{
			if (Held.Value.bAnalog && !Held.Value.Action.IsValid())
			{
				SendAxis(*Controller, Held.Value.Key, Held.Value.KeyValue, DeltaSeconds);
			}
		}
	}
	const double Now = World->GetTimeSeconds();
	for (TPair<int32, TSharedPtr<FOperation>>& Entry : Operations)
	{
		if (Entry.Value->bDone || Entry.Value->Kind != EOperationKind::Input)
		{
			continue;
		}
		for (FScheduledInput& Input : Entry.Value->Inputs)
		{
			if (Input.State == FScheduledInput::EState::Scheduled && Now >= Input.StartWorld - KINDA_SMALL_NUMBER)
			{
				BeginInput(Input, *Controller, Now);
			}
			else if (Input.State == FScheduledInput::EState::Active && Input.ActiveTicks >= 1 && Now >= Input.EndWorld - KINDA_SMALL_NUMBER)
			{
				Input.ActualEnd = Now;
				if (Input.Mode == FScheduledInput::EMode::Down)
				{
					// Left held: the request reports it after two more frames, the key or action stays down.
					FHeldInput& Held = HeldInputs.Add(Input.HeldId);
					Held.Action = Input.Action;
					Held.ActionValue = Input.ActionValue;
					Held.Key = Input.Key;
					Held.KeyValue = Input.KeyValue;
					Held.bAnalog = Input.bAnalog;
					Held.Since = Input.ActualStart;
					Input.bKeptDown = true;
					Input.State = FScheduledInput::EState::Released;
				}
				else
				{
					ReleaseInput(Input, Controller);
				}
				continue;
			}
			if (Input.State == FScheduledInput::EState::Active && Input.bAnalog && !Input.Action.IsValid())
			{
				SendAxis(*Controller, Input.Key, Input.KeyValue, DeltaSeconds);
			}
		}
	}
}

void FPlayManager::OnWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World == nullptr || GEditor == nullptr || World != GEditor->PlayWorld || Operations.IsEmpty())
	{
		return;
	}
	APlayerController* Controller = GetLocalPlayerController(*World);
	UEnhancedInputLocalPlayerSubsystem* Subsystem = Controller != nullptr ? GetInputSubsystem(*Controller) : nullptr;
	const UEnhancedPlayerInput* PlayerInput = Subsystem != nullptr ? Subsystem->GetPlayerInput() : nullptr;
	for (TPair<int32, TSharedPtr<FOperation>>& Entry : Operations)
	{
		if (Entry.Value->bDone || Entry.Value->Kind != EOperationKind::Input)
		{
			continue;
		}
		for (FScheduledInput& Input : Entry.Value->Inputs)
		{
			if (Input.State != FScheduledInput::EState::Active && Input.State != FScheduledInput::EState::Released)
			{
				continue;
			}
			if (PlayerInput != nullptr)
			{
				for (FScheduledInput::FWatched& Watched : Input.Watched)
				{
					const FInputActionInstance* Instance = Watched.Action.IsValid() ? PlayerInput->FindActionInstanceData(Watched.Action.Get()) : nullptr;
					if (Instance == nullptr)
					{
						continue;
					}
					const ETriggerEvent Event = Instance->GetTriggerEvent();
					if (Event != ETriggerEvent::None)
					{
						Watched.Events.Add(TriggerEventName(Event));
					}
					// Started fires in the frame the action leaves None, whose state reads Triggered or Ongoing.
					if ((Event == ETriggerEvent::Triggered || Event == ETriggerEvent::Ongoing)
						&& (Watched.LastEvent == ETriggerEvent::None || Watched.LastEvent == ETriggerEvent::Completed || Watched.LastEvent == ETriggerEvent::Canceled))
					{
						Watched.Events.Add(TriggerEventName(ETriggerEvent::Started));
					}
					Watched.LastEvent = Event;
					if (Event == ETriggerEvent::Triggered)
					{
						++Watched.TriggeredTicks;
					}
					if (Event == ETriggerEvent::Triggered || Instance->GetLastTriggeredWorldTime() > Watched.TriggeredBefore)
					{
						Watched.bTriggered = true;
					}
				}
			}
			if (Input.State == FScheduledInput::EState::Active)
			{
				++Input.ActiveTicks;
			}
			else if (++Input.ReleasedTicks >= 2)
			{
				// Injected values reach the action a frame late: two frames after the release, Completed is recorded too.
				Input.State = FScheduledInput::EState::Done;
			}
		}
	}
	UpdateOperations(World, true);
}

void FPlayManager::OnPreBeginPie(bool bSimulating)
{
	FlushPendingLog();
	SessionLogSeq = FPlayLog::Get().GetLastSeq();
	SessionStartReal = FPlatformTime::Seconds();
	UnpausedReal = 0.0;
	LastCoreTickReal = 0.0;
	HeldInputs.Reset();
	bInputFrozen = false;
	FrozenController.Reset();
	if (!bToolSession)
	{
		SessionMode = bSimulating ? TEXT("simulate") : FString();
	}
}

// The editor has created the play world and begun play (BeginPlay ran, the player has its pawn) and has not
// ticked it yet: a paused start pauses here, so the game is at world time 0, and the start's console
// commands run here.
void FPlayManager::OnPostPieStarted(bool bSimulating)
{
	UWorld* World = GetPlayWorld();
	if (World == nullptr)
	{
		return;
	}
	for (TPair<int32, TSharedPtr<FOperation>>& Entry : Operations)
	{
		FOperation& Operation = *Entry.Value;
		if (Operation.bDone || Operation.Kind != EOperationKind::Start)
		{
			continue;
		}
		if (Operation.bStartPaused)
		{
			GEditor->SetPIEWorldsPaused(true);
			Operation.bPausedAtStart = true;
		}
		APlayerController* Controller = GetLocalPlayerController(*World);
		for (int32 Index = 0; Index < Operation.StartCommands.Num(); ++Index)
		{
			if (Controller == nullptr)
			{
				Operation.Warnings.Add(MakeWarning(FString::Printf(TEXT("/console/%d"), Index), TEXT("Play.NoPlayer"),
					TEXT("The game has no local player yet to type the start console commands; they did not run.")));
				break;
			}
			TArray<FAssetDocumentDiagnostic> Diagnostics;
			TSharedPtr<FJsonObject> Ran = RunConsoleCommand(*World, *Controller, Operation.StartCommands[Index],
				FString::Printf(TEXT("/console/%d"), Index), Diagnostics);
			for (FAssetDocumentDiagnostic& Diagnostic : Diagnostics)
			{
				Diagnostic.Severity = TEXT("warning");
				Operation.Warnings.Add(Diagnostic);
			}
			if (Ran.IsValid())
			{
				Operation.StartConsole.Add(MakeShared<FJsonValueObject>(Ran));
			}
		}
	}
}

void FPlayManager::OnEndPie(bool bSimulating)
{
	HeldInputs.Reset();
	bInputFrozen = false;
	FrozenController.Reset();
	for (TPair<int32, TSharedPtr<FOperation>>& Entry : Operations)
	{
		for (FScheduledInput& Input : Entry.Value->Inputs)
		{
			if (Input.State == FScheduledInput::EState::Active)
			{
				Input.State = FScheduledInput::EState::Released;
			}
		}
	}
	bToolSession = false;
}

bool FPlayManager::OnCoreTick(float DeltaTime)
{
	const double NowReal = FPlatformTime::Seconds();
	if (GEditor != nullptr && GEditor->PlayWorld != nullptr && GEditor->PlayWorld->HasBegunPlay())
	{
		if (LastCoreTickReal > 0.0 && !GEditor->PlayWorld->bDebugPauseExecution)
		{
			UnpausedReal += NowReal - LastCoreTickReal;
		}
		LastCoreTickReal = NowReal;
	}
	if (!Operations.IsEmpty())
	{
		UWorld* World = GetPlayWorld();
		// Waits are evaluated after each world tick; here only what needs no tick: start, stop,
		// timeouts and a session that ended.
		TArray<TSharedPtr<FOperation>> Current;
		Operations.GenerateValueArray(Current);
		for (const TSharedPtr<FOperation>& Operation : Current)
		{
			if (Operation->bDone)
			{
				continue;
			}
			if (Operation->Kind == EOperationKind::Start)
			{
				UpdateStart(*Operation);
			}
			else if (Operation->Kind == EOperationKind::Stop)
			{
				UpdateStop(*Operation);
			}
			else if (World == nullptr || FPlatformTime::Seconds() > Operation->DeadlineReal)
			{
				if (Operation->Kind == EOperationKind::Wait)
				{
					UpdateWait(*Operation, World, false);
				}
				else
				{
					UpdateInput(*Operation, World);
				}
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// console

// "ke <Object> <Function> <args>" (KismetEvent) finds <Object> by name among all loaded objects, so the name of a
// placed actor hits the editor's copy as often as the game's. The target is resolved in the play world
// first (an actor name, label or "player"; any other play-world object by name) and replaced by its full
// path; a class name or "*" (every object of the play world) is left to the command.
bool RewriteKismetEventTarget(UWorld& World, const FString& Line, FString& OutLine, FString& OutNote, FString& OutError)
{
	OutLine = Line;
	const FString Trimmed = Line.TrimStartAndEnd();
	FString Word;
	FString Rest;
	if (!Trimmed.Split(TEXT(" "), &Word, &Rest) || !(Word.Equals(TEXT("ke"), ESearchCase::IgnoreCase) || Word.Equals(TEXT("kismetevent"), ESearchCase::IgnoreCase)))
	{
		return true;
	}
	Rest.TrimStartInline();
	FString Target;
	FString Arguments;
	if (!Rest.Split(TEXT(" "), &Target, &Arguments))
	{
		Target = Rest;
	}
	if (Target.IsEmpty() || Target == TEXT("*"))
	{
		return true;
	}
	TArray<UObject*> Named;
	if (!Target.Contains(TEXT(".")) && !Target.Contains(TEXT("/")))
	{
		StaticFindAllObjects(Named, UObject::StaticClass(), *Target);
		if (Named.ContainsByPredicate([](const UObject* Object) { return Object != nullptr && Object->IsA<UClass>(); }))
		{
			return true;
		}
	}
	UObject* Resolved = nullptr;
	const FPlayActorResolution Actor = ResolvePlayActor(World, MakeShared<FJsonValueString>(Target));
	if (Actor.Actor != nullptr)
	{
		Resolved = Actor.Actor;
	}
	else
	{
		for (UObject* Object : Named)
		{
			if (Object != nullptr && Object->GetWorld() == &World)
			{
				Resolved = Object;
				break;
			}
		}
	}
	if (Resolved == nullptr)
	{
		if (Actor.Code == TEXT("Play.ActorRefAmbiguous"))
		{
			OutError = Actor.Message;
			return false;
		}
		UObject* Outside = Target.Contains(TEXT(".")) ? FindObject<UObject>(nullptr, *Target) : (Named.IsEmpty() ? nullptr : Named[0]);
		if (Outside != nullptr && Outside->GetWorld() != &World)
		{
			OutError = FString::Printf(TEXT("ke target '%s' is not in the play world (it names %s); use an actor name, label or path that "
				"query_play returned."), *Target, *Outside->GetPathName());
			return false;
		}
		return true;
	}
	const FString Path = Resolved->GetPathName();
	OutLine = FString::Printf(TEXT("%s %s%s"), *Word, *Path, Arguments.IsEmpty() ? TEXT("") : *(TEXT(" ") + Arguments));
	if (Path != Target)
	{
		OutNote = FString::Printf(TEXT("ke target '%s' -> %s"), *Target, *Path);
	}
	return true;
}

TSharedPtr<FJsonObject> FPlayManager::RunConsoleCommand(UWorld& World, APlayerController& Controller, const FString& Command, const FString& Path,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	TArray<FString> Lines;
	Command.ParseIntoArray(Lines, TEXT("|"));
	TArray<FString> Effective;
	TArray<FString> Notes;
	for (const FString& Line : Lines)
	{
		FString Rewritten;
		FString Note;
		FString Error;
		if (!RewriteKismetEventTarget(World, Line, Rewritten, Note, Error))
		{
			OutDiagnostics.Add(MakePlayDiagnostic(Path, TEXT("Play.ActorNotFound"), Error));
			return nullptr;
		}
		Effective.Add(Rewritten.TrimStartAndEnd());
		if (!Note.IsEmpty())
		{
			Notes.Add(Note);
		}
	}
	FlushPendingLog();
	const uint64 Since = FPlayLog::Get().GetLastSeq();
	FString Output;
	{
		FPlayWorldScope PlayWorld(World);
		// Not written to the log: the player's console output (what the command printed) is returned.
		Output = Controller.ConsoleCommand(FString::Join(Effective, TEXT("|")), false);
	}
	FlushPendingLog();
	// What the command logged instead of printing (most game commands log).
	FPlayLogFilter Filter;
	Filter.Since = Since;
	Filter.MaxVerbosity = ELogVerbosity::Verbose;
	int32 Skipped = 0;
	uint64 Oldest = 0;
	TArray<FString> Logged;
	for (const FPlayLogEntry& Entry : FPlayLog::Get().Query(Filter, 20, Skipped, Oldest))
	{
		Logged.Add(FString::Printf(TEXT("%s: %s"), *Entry.Category.ToString(), *Entry.Message.Left(300)));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("command"), Command);
	if (!Notes.IsEmpty())
	{
		Result->SetStringField(TEXT("ran"), FString::Join(Effective, TEXT(" | ")));
		Result->SetArrayField(TEXT("resolved"), PlayStringArray(Notes));
	}
	Output.TrimEndInline();
	Result->SetStringField(TEXT("output"), Output.Left(4000));
	if (!Logged.IsEmpty())
	{
		Result->SetArrayField(TEXT("logged"), PlayStringArray(Logged));
	}
	if (Output.Contains(TEXT("Command not recognized")))
	{
		OutDiagnostics.Add(MakeWarning(Path, TEXT("Play.CommandNotRecognized"), FString::Printf(TEXT("The game did not recognize '%s' (see output)."),
			*Command)));
	}
	return Result;
}

FLevelToolResult FPlayManager::Console(const FJsonObject& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	const TCHAR* const Fields[] = {TEXT("action"), TEXT("command")};
	CheckFields(Request, Fields, FString(), TEXT("play console"), Diagnostics);
	FString Command;
	if (!Request.TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/command"), TEXT("console needs 'command', e.g. \"slomo 0.5\", \"god\", \"showdebug ai\".")));
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	UWorld* World = GetPlayWorld();
	if (World == nullptr)
	{
		return NoSession();
	}
	APlayerController* Controller = GetLocalPlayerController(*World);
	if (Controller == nullptr)
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.NoPlayer"), TEXT("The play session has no local player to type the command.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	const FString Refused = RefusedConsoleWord(Command);
	if (!Refused.IsEmpty())
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/command"), TEXT("Play.CommandRefused"), FString::Printf(
			TEXT("'%s' is refused here: it would affect the editor, not only the game."), *Refused)));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	FlushPendingLog();
	const uint64 Since = FPlayLog::Get().GetLastSeq();
	const TSharedPtr<FJsonObject> Ran = RunConsoleCommand(*World, *Controller, Command.TrimStartAndEnd(), TEXT("/command"), Diagnostics);
	if (!Ran.IsValid())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	for (const auto& Field : Ran->Values)
	{
		Payload->SetField(Field.Key, Field.Value);
	}
	Payload->SetObjectField(TEXT("log"), LogSummaryJson(Since, SessionStartReal));
	Payload->SetObjectField(TEXT("session"), Status());
	FLevelToolResult Result = FLevelToolResult::Success(FString::Printf(TEXT("Ran '%s'."), *Command), Payload);
	Result.Diagnostics = MoveTemp(Diagnostics);
	return Result;
}

FLevelToolResult FPlayManager::CallOrSet(const FJsonObject& Request, bool bCall)
{
	UWorld* World = GetPlayWorld();
	if (World == nullptr)
	{
		return NoSession();
	}
	FlushPendingLog();
	const uint64 Since = FPlayLog::Get().GetLastSeq();
	FLevelToolResult Result = bCall ? RunPlayCall(*World, Request) : RunPlaySet(*World, Request);
	if (!Result.Payload.IsValid())
	{
		Result.Payload = MakeShared<FJsonObject>();
	}
	// What the call logged (Print String, Accessed None) and the session after it.
	Result.Payload->SetObjectField(TEXT("log"), LogSummaryJson(Since, SessionStartReal));
	Result.Payload->SetObjectField(TEXT("session"), Status());
	return Result;
}

FLevelToolResult FPlayManager::ResetInputSettings(const FJsonObject& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	const TCHAR* const Fields[] = {TEXT("action")};
	if (!CheckFields(Request, Fields, FString(), TEXT("play reset_input_settings"), Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	FLevelToolResult Result = ResetPlayInputSettings(GetPlayWorld());
	if (!Result.Payload.IsValid())
	{
		Result.Payload = MakeShared<FJsonObject>();
	}
	Result.Payload->SetObjectField(TEXT("session"), Status());
	return Result;
}

FLevelToolResult FPlayManager::Run(const TSharedPtr<FJsonObject>& Request)
{
	check(IsInGameThread());
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	FString Action;
	if (!Request.IsValid() || !Request->TryGetStringField(TEXT("action"), Action))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/action"), TEXT("'action' is start, stop, status, pause, resume, wait, input, console, call, set, reset_input_settings or poll.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (GEditor == nullptr)
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.EditorBusy"), TEXT("The editor is not available.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (Action == TEXT("start")) { return Start(*Request); }
	if (Action == TEXT("stop")) { return Stop(*Request); }
	if (Action == TEXT("pause")) { return Pause(true); }
	if (Action == TEXT("resume")) { return Pause(false); }
	if (Action == TEXT("wait")) { return Wait(*Request); }
	if (Action == TEXT("input")) { return HandleInput(*Request); }
	if (Action == TEXT("console")) { return Console(*Request); }
	if (Action == TEXT("call")) { return CallOrSet(*Request, true); }
	if (Action == TEXT("set")) { return CallOrSet(*Request, false); }
	if (Action == TEXT("poll")) { return Poll(*Request); }
	if (Action == TEXT("reset_input_settings")) { return ResetInputSettings(*Request); }
	if (Action == TEXT("status"))
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetObjectField(TEXT("session"), Status());
		return FLevelToolResult::Success(FString::Printf(TEXT("Play session: %s."), *SessionState()), Payload);
	}
	Diagnostics.Add(PlayInvalidRequest(TEXT("/action"), FString::Printf(
		TEXT("Unknown action '%s': start, stop, status, pause, resume, wait, input, console, call, set, reset_input_settings or poll."), *Action)));
	return FLevelToolResult::Failure(MoveTemp(Diagnostics));
}
} // namespace

FLevelToolResult RunPlay(const TSharedPtr<FJsonObject>& Request)
{
	return FPlayManager::Get().Run(Request);
}

FLevelToolResult RunPlayFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	FLevelToolResult Failure;
	return ParsePlayBody(RequestBody, Json, Failure) ? RunPlay(Json) : Failure;
}

TSharedRef<FJsonObject> MakeSessionStatusJson()
{
	return FPlayManager::Get().Status();
}

void GetSessionLogBase(uint64& OutSeq, double& OutRealTime)
{
	FPlayManager::Get().GetLogBase(OutSeq, OutRealTime);
}

void FlushPendingLog()
{
	if (GLog != nullptr && IsInGameThread())
	{
		GLog->FlushThreadedLogs();
	}
}

void StartupPlayTools()
{
	FPlayManager::Get().Startup();
}

void ShutdownPlayTools()
{
	FPlayManager::Get().Shutdown();
}
}
