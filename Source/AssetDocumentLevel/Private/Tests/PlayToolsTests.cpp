// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "CoreMinimal.h"
#include "AssetDocumentSettings.h"
#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameMapsSettings.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"
#include "Play/PlayCommandGuard.h"
#include "Play/PlayQuery.h"
#include "Play/PlaySession.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// Play In Editor runs on Mac and on the Windows test PC (-RenderOffScreen): the PIE window renders
// off screen there, which the game-view screenshot reads back without its Slate widgets.
namespace AssetDocumentPlayToolsTests
{
using namespace AssetDocumentLevelTests;
using UE::AssetDocumentLevel::Private::FLevelToolResult;
namespace Play = UE::AssetDocumentLevel::Private::Play;

const TCHAR* const PrintText = TEXT("PlayTest action fired");

bool HasCode(const FLevelToolResult& Result, const TCHAR* Code)
{
	return Result.FindDiagnostic(Code) != nullptr;
}

FString Codes(const FLevelToolResult& Result)
{
	TArray<FString> Out;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Out.Add(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	return FString::Join(Out, TEXT(" | "));
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

FLevelToolResult Query(const FString& Body)
{
	return Play::RunPlayQueryFromBody(Body);
}

FString SessionState(const FLevelToolResult& Result)
{
	const TSharedPtr<FJsonObject>* Session = nullptr;
	return Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("session"), Session) && Session != nullptr
		? (*Session)->GetStringField(TEXT("state")) : FString();
}

// One play request run to completion, polled every frame as the MCP tool polls it.
class FPlayRequestCommand final : public IAutomationLatentCommand
{
public:
	FPlayRequestCommand(FString InBody, TFunction<void(const FLevelToolResult&)> InOnDone)
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

UInputAction* NewInputAction(const TCHAR* Name)
{
	UInputAction* Action = NewObject<UInputAction>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UInputAction::StaticClass(), FName(Name)));
	Action->ValueType = EInputActionValueType::Boolean;
	return Action;
}

// A Pawn Blueprint: "<Action> Triggered -> set bActionFired -> Print String". bBroken adds an
// Activate call after it without a target (a Pawn is not an Actor Component), which does not compile.
UBlueprint* NewPawnBlueprint(UInputAction* Action, bool bBroken)
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(APawn::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BP_PlayTestPawn")), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bActionFired"), BoolType);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);

	FGraphNodeCreator<UK2Node_EnhancedInputAction> EventCreator(*Graph);
	UK2Node_EnhancedInputAction* Event = EventCreator.CreateNode(false);
	Event->InputAction = Action;
	EventCreator.Finalize();

	FGraphNodeCreator<UK2Node_VariableSet> SetCreator(*Graph);
	UK2Node_VariableSet* Set = SetCreator.CreateNode(false);
	Set->VariableReference.SetSelfMember(TEXT("bActionFired"));
	SetCreator.Finalize();

	FGraphNodeCreator<UK2Node_CallFunction> PrintCreator(*Graph);
	UK2Node_CallFunction* Print = PrintCreator.CreateNode(false);
	Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
	PrintCreator.Finalize();

	if (UEdGraphPin* Value = Set->FindPin(TEXT("bActionFired")))
	{
		Value->DefaultValue = TEXT("true");
	}
	if (UEdGraphPin* Text = Print->FindPin(TEXT("InString")))
	{
		Text->DefaultValue = PrintText;
	}
	if (UEdGraphPin* Triggered = Event->FindPin(TEXT("Triggered")))
	{
		Triggered->MakeLinkTo(Set->GetExecPin());
	}
	Set->GetThenPin()->MakeLinkTo(Print->GetExecPin());
	if (bBroken)
	{
		FGraphNodeCreator<UK2Node_CallFunction> ActivateCreator(*Graph);
		UK2Node_CallFunction* Activate = ActivateCreator.CreateNode(false);
		Activate->SetFromFunction(UActorComponent::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UActorComponent, Activate)));
		ActivateCreator.Finalize();
		Print->GetThenPin()->MakeLinkTo(Activate->GetExecPin());
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	return Blueprint;
}

// Keeps a test Blueprint out of every later play session's compile check.
void DiscardBlueprint(UBlueprint* Blueprint)
{
	if (Blueprint == nullptr)
	{
		return;
	}
	Blueprint->Status = BS_UpToDate;
	Blueprint->bDisplayCompilePIEWarning = false;
	Blueprint->ClearFlags(RF_Public | RF_Standalone);
	Blueprint->MarkAsGarbage();
}

struct FSessionState
{
	TStrongObjectPtr<UInputAction> Action;
	TStrongObjectPtr<UInputMappingContext> Context;
	TStrongObjectPtr<UBlueprint> Blueprint;
	FString ActionPath;
	int32 PrintsAfterAction = 0;
};

int32 CountPrints()
{
	const FLevelToolResult Log = Query(FString::Printf(TEXT("{\"what\": \"log\", \"category\": \"LogBlueprintUserMessages\", \"text\": \"%s\"}"), PrintText));
	return Log.bSuccess && Log.Payload.IsValid() ? Log.Payload->GetArrayField(TEXT("entries")).Num() : -1;
}

TSharedPtr<FJsonObject> FirstActor(const FLevelToolResult& Result)
{
	if (!Result.bSuccess || !Result.Payload.IsValid())
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>& Actors = Result.Payload->GetArrayField(TEXT("actors"));
	return Actors.IsEmpty() ? nullptr : Actors[0]->AsObject();
}

TSharedPtr<FJsonObject> FirstInputAction(const FLevelToolResult& Result);

// Adds "Echo(Value: Vector) -> Result: Vector" (the input wired to the output) and recompiles.
void AddEchoFunction(UBlueprint* Blueprint)
{
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("Echo"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);
	TArray<UK2Node_FunctionEntry*> Entries;
	Graph->GetNodesOfClass(Entries);
	if (Entries.IsEmpty())
	{
		return;
	}
	FEdGraphPinType VectorType;
	VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	UEdGraphPin* In = Entries[0]->CreateUserDefinedPin(TEXT("Value"), VectorType, EGPD_Output);
	TArray<UK2Node_FunctionResult*> Results;
	Graph->GetNodesOfClass(Results);
	UK2Node_FunctionResult* Result = Results.IsEmpty() ? nullptr : Results[0];
	if (Result == nullptr)
	{
		FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*Graph);
		Result = ResultCreator.CreateNode();
		ResultCreator.Finalize();
	}
	UEdGraphPin* Out = Result->CreateUserDefinedPin(TEXT("Result"), VectorType, EGPD_Input);
	Entries[0]->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(Result->FindPin(UEdGraphSchema_K2::PN_Execute));
	if (In != nullptr && Out != nullptr)
	{
		In->MakeLinkTo(Out);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
}

TSharedPtr<FJsonObject> ObjectAt(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	return Json.IsValid() && Json->TryGetObjectField(Field, Found) && Found != nullptr ? *Found : nullptr;
}

TSharedPtr<FJsonObject> Session(const FLevelToolResult& Result)
{
	return ObjectAt(Result.Payload, TEXT("session"));
}

TArray<FString> InputEvents(const FLevelToolResult& Result)
{
	TArray<FString> Events;
	if (const TSharedPtr<FJsonObject> Action = FirstInputAction(Result))
	{
		Action->TryGetStringArrayField(TEXT("events"), Events);
	}
	return Events;
}

AActor* FindPlayActor(const FString& Label)
{
	UWorld* World = GEditor->PlayWorld;
	if (World == nullptr)
	{
		return nullptr;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel(false) == Label)
		{
			return *It;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FirstInputAction(const FLevelToolResult& Result)
{
	if (!Result.Payload.IsValid() || !Result.Payload->HasTypedField<EJson::Array>(TEXT("inputs")))
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>& Inputs = Result.Payload->GetArrayField(TEXT("inputs"));
	if (Inputs.IsEmpty())
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>& Actions = Inputs[0]->AsObject()->GetArrayField(TEXT("actions"));
	return Actions.IsEmpty() ? nullptr : Actions[0]->AsObject();
}
} // namespace AssetDocumentPlayToolsTests

// Requests that need no play session: validation and the refusals without one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayRequestsTest,
	"UEAssetDoc.Level.Play.Requests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayRequestsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPlayToolsTests;
	if (!TestNull(TEXT("no play session is running"), GEditor->PlayWorld.Get()))
	{
		return false;
	}
	const FLevelToolResult Status = Play::RunPlayFromBody(TEXT("{\"action\": \"status\"}"));
	TestTrue(TEXT("status works without a session"), Status.bSuccess);
	TestEqual(TEXT("status: stopped"), SessionState(Status), FString(TEXT("stopped")));
	TestTrue(TEXT("no action"), HasCode(Play::RunPlayFromBody(TEXT("{}")), TEXT("Play.InvalidRequest")));
	TestTrue(TEXT("unknown action"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"jump\"}")), TEXT("Play.InvalidRequest")));
	TestTrue(TEXT("not JSON"), HasCode(Play::RunPlayFromBody(TEXT("[1")), TEXT("Play.InvalidRequest")));
	TestTrue(TEXT("unknown start mode"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"start\", \"mode\": \"vr\"}")), TEXT("Play.InvalidRequest")));
	TestTrue(TEXT("unknown start field"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"start\", \"players\": 2}")), TEXT("Play.InvalidRequest")));
	TestNull(TEXT("refused starts start nothing"), GEditor->PlayWorld.Get());
	TestFalse(TEXT("refused starts queue nothing"), GEditor->IsPlaySessionRequestQueued());
	TestTrue(TEXT("wait without session"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"wait\", \"seconds\": 1}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("wait without seconds or until"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"wait\"}")), TEXT("Play.InvalidRequest")));
	TestTrue(TEXT("input without session"),
		HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"SpaceBar\"}]}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("console without session"),
		HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"console\", \"command\": \"stat fps\"}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("pause without session"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"pause\"}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("poll of an unknown operation"),
		HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"poll\", \"id\": 999999}")), TEXT("Play.UnknownOperation")));
	const FLevelToolResult Stop = Play::RunPlayFromBody(TEXT("{\"action\": \"stop\"}"));
	TestTrue(TEXT("stop without a session is a no-op"), Stop.bSuccess);
	TestTrue(TEXT("query_play without session"), HasCode(Query(TEXT("{}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("query_play unknown field"), HasCode(Query(TEXT("{\"actor\": \"player\"}")), TEXT("Play.InvalidRequest")));
	TestTrue(TEXT("the log is readable without a session"), Query(TEXT("{\"what\": \"log\", \"since\": \"all\", \"limit\": 1}")).bSuccess);
	TestTrue(TEXT("bad log verbosity"), HasCode(Query(TEXT("{\"what\": \"log\", \"verbosity\": \"loud\"}")), TEXT("Play.InvalidRequest")));
	TestTrue(TEXT("game screenshot without session"),
		HasCode(Play::RunPlayScreenshotFromBody(TEXT("{\"view\": \"game\"}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("bad screenshot view"), HasCode(Play::RunPlayScreenshotFromBody(TEXT("{\"view\": \"map\"}")), TEXT("Play.InvalidRequest")));
	for (const TCHAR* Body : {TEXT("{\"format\": \"gif\"}"), TEXT("{\"format\": \"png\", \"quality\": 50}"), TEXT("{\"quality\": 0}"),
		TEXT("{\"quality\": 70.5}"), TEXT("{\"max_width\": 10}"), TEXT("{\"max_width\": \"wide\"}")})
	{
		TestTrue(*FString::Printf(TEXT("bad screenshot options %s"), Body), HasCode(Play::RunPlayScreenshotFromBody(Body), TEXT("Play.InvalidRequest")));
	}

	// Call / set / effects need a session; start's console is checked before anything starts.
	TestTrue(TEXT("call without session"), HasCode(Play::RunPlayFromBody(
		TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"Jump\"}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("set without session"), HasCode(Play::RunPlayFromBody(
		TEXT("{\"action\": \"set\", \"actor\": \"player\", \"values\": {\"Health\": 1}}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("effects without session"), HasCode(Query(TEXT("{\"what\": \"effects\"}")), TEXT("Play.NoSession")));
	TestTrue(TEXT("a refused start console command"), HasCode(Play::RunPlayFromBody(
		TEXT("{\"action\": \"start\", \"console\": [\"slomo 0.5\", \"quit\"]}")), TEXT("Play.CommandRefused")));
	TestTrue(TEXT("start console needs a player"), HasCode(Play::RunPlayFromBody(
		TEXT("{\"action\": \"start\", \"mode\": \"simulate\", \"console\": \"god\"}")), TEXT("Play.InvalidRequest")));
	TestNull(TEXT("refused starts start nothing (console)"), GEditor->PlayWorld.Get());
	TestFalse(TEXT("refused starts queue nothing (console)"), GEditor->IsPlaySessionRequestQueued());
	// The log fields are flat fields of what=log, not a nested "log" object.
	const FLevelToolResult Nested = Query(TEXT("{\"what\": \"log\", \"log\": {\"text\": \"x\"}}"));
	const FAssetDocumentDiagnostic* NestedError = Nested.FindDiagnostic(TEXT("Play.InvalidRequest"));
	if (TestNotNull(TEXT("a nested log object is refused"), NestedError))
	{
		TestTrue(*FString::Printf(TEXT("the message lists what=log's fields (%s)"), *NestedError->Message),
			NestedError->Message.Contains(TEXT("With what=log: since / category / verbosity / text / limit")));
		TestTrue(TEXT("and says they are flat"), NestedError->Message.Contains(TEXT("flat")));
	}
	TestTrue(TEXT("an actor field with what=log is refused"), HasCode(Query(TEXT("{\"what\": \"log\", \"class\": \"Pawn\"}")), TEXT("Play.InvalidRequest")));
	TestTrue(TEXT("unknown what"), HasCode(Query(TEXT("{\"what\": \"particles\"}")), TEXT("Play.InvalidRequest")));
	return true;
}

// Play refuses to start while a Blueprint has compile errors, naming it and its errors, instead of
// the editor's modal prompt; allow_blueprint_errors plays anyway.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayBlueprintErrorsTest,
	"UEAssetDoc.Level.Play.BlueprintErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayBlueprintErrorsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPlayToolsTests;
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	TSharedRef<FSessionState> State = MakeShared<FSessionState>();
	State->Action.Reset(NewInputAction(TEXT("IA_PlayTestBroken")));
	// The compiler logs the Blueprint's error each time it compiles it.
	AddExpectedError(TEXT("must have a connection"), EAutomationExpectedErrorFlags::Contains, 0);
	State->Blueprint.Reset(NewPawnBlueprint(State->Action.Get(), true));
	if (!TestEqual(TEXT("the test Blueprint does not compile"), static_cast<int32>(State->Blueprint->Status), static_cast<int32>(BS_Error)))
	{
		DiscardBlueprint(State->Blueprint.Get());
		return false;
	}
	const FLevelToolResult Refused = Play::RunPlayFromBody(TEXT("{\"action\": \"start\"}"));
	TestFalse(TEXT("start is refused"), Refused.bSuccess);
	const FAssetDocumentDiagnostic* Errors = Refused.FindDiagnostic(TEXT("Play.BlueprintCompileErrors"));
	if (TestNotNull(TEXT("Play.BlueprintCompileErrors"), Errors))
	{
		TestTrue(TEXT("the message names the Blueprint"), Errors->Message.Contains(State->Blueprint->GetName()));
		bool bListed = false;
		if (TestTrue(TEXT("details list the Blueprints"), Errors->Details.IsValid() && Errors->Details->HasTypedField<EJson::Array>(TEXT("blueprints"))))
		{
			for (const TSharedPtr<FJsonValue>& Entry : Errors->Details->GetArrayField(TEXT("blueprints")))
			{
				const TSharedPtr<FJsonObject> Item = Entry->AsObject();
				if (Item->GetStringField(TEXT("blueprint")) == State->Blueprint->GetPathName())
				{
					bListed = true;
					TestTrue(TEXT("its compile errors are listed"), Item->GetArrayField(TEXT("errors")).Num() > 0);
				}
			}
		}
		TestTrue(TEXT("the broken Blueprint is listed"), bListed);
	}
	TestFalse(TEXT("nothing was queued"), GEditor->IsPlaySessionRequestQueued());
	TestNull(TEXT("no play world"), GEditor->PlayWorld.Get());

	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"start\", \"allow_blueprint_errors\": true}"),
		[this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("allow_blueprint_errors plays anyway (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(TEXT("with a warning"), HasCode(Result, TEXT("Play.BlueprintCompileErrors")));
		TestNotNull(TEXT("the session runs"), GEditor->PlayWorld.Get());
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"stop\"}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(TEXT("stopped"), Result.bSuccess);
		TestNull(TEXT("the session ended"), GEditor->PlayWorld.Get());
		DiscardBlueprint(State->Blueprint.Get());
		State->Blueprint.Reset();
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}));
	return true;
}

// A playtest as an MCP client does it: start, observe, inject an Input Action and a key, see the
// Blueprint react (variable and Print String), wait on conditions, pause, screenshot, stop.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlaySessionTest,
	"UEAssetDoc.Level.Play.Session",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlaySessionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPlayToolsTests;
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("editor world"), World))
	{
		return false;
	}
	TSharedRef<FSessionState> State = MakeShared<FSessionState>();
	State->Action.Reset(NewInputAction(TEXT("IA_PlayTestFire")));
	State->ActionPath = State->Action->GetPathName();
	State->Context.Reset(NewObject<UInputMappingContext>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UInputMappingContext::StaticClass(), TEXT("IMC_PlayTest"))));
	State->Context->MapKey(State->Action.Get(), EKeys::J);
	State->Blueprint.Reset(NewPawnBlueprint(State->Action.Get(), false));
	if (!TestTrue(TEXT("the test Blueprint compiles"), State->Blueprint->IsUpToDate()))
	{
		DiscardBlueprint(State->Blueprint.Get());
		return false;
	}
	APawn* Pawn = Cast<APawn>(SpawnActor(*World->PersistentLevel, State->Blueprint->GeneratedClass, FVector(0.0, 0.0, 300.0), TEXT("PlayTestPawn")));
	AActor* Crate = SpawnActor(*World->PersistentLevel, AStaticMeshActor::StaticClass(), FVector(500.0, 0.0, 0.0), TEXT("PlayTestCrate"));
	if (!TestNotNull(TEXT("pawn placed"), Pawn) || !TestNotNull(TEXT("crate placed"), Crate))
	{
		DiscardBlueprint(State->Blueprint.Get());
		return false;
	}
	Pawn->AutoPossessPlayer = EAutoReceiveInput::Player0;
	Crate->Tags.Add(TEXT("crate"));

	// Started paused: the game runs only during input and wait, never while the caller thinks.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"start\", \"paused\": true}"), [this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("start succeeds (%s)"), *Codes(Result)), Result.bSuccess);
		TestEqual(TEXT("state paused"), SessionState(Result), FString(TEXT("paused")));
		TestNotNull(TEXT("a play world"), GEditor->PlayWorld.Get());
		TestTrue(TEXT("a second start is refused"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"start\"}")), TEXT("Play.AlreadyRunning")));
	}));

	// Observation: the player, a placed actor by tag, values by name, editor paths.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"status\"}"), [this](const FLevelToolResult& Status)
	{
		const TSharedPtr<FJsonObject>* Session = nullptr;
		const TSharedPtr<FJsonObject>* Player = nullptr;
		const TSharedPtr<FJsonObject>* PawnJson = nullptr;
		if (TestTrue(TEXT("status names the player's pawn"), Status.Payload.IsValid() && Status.Payload->TryGetObjectField(TEXT("session"), Session)
			&& (*Session)->TryGetObjectField(TEXT("player"), Player) && (*Player)->TryGetObjectField(TEXT("pawn"), PawnJson)))
		{
			TestEqual(TEXT("the auto-possessed pawn"), (*PawnJson)->GetStringField(TEXT("label")), FString(TEXT("PlayTestPawn")));
		}
		const FLevelToolResult Player0 = Query(TEXT("{\"actors\": [\"player\"], \"properties\": [\"bActionFired\", \"NoSuchValue\"]}"));
		const TSharedPtr<FJsonObject> PlayerJson = FirstActor(Player0);
		if (TestNotNull(*FString::Printf(TEXT("query the player (%s)"), *Codes(Player0)), PlayerJson.Get()))
		{
			TestFalse(TEXT("bActionFired starts false"), PlayerJson->GetObjectField(TEXT("values"))->GetBoolField(TEXT("bActionFired")));
			TestTrue(TEXT("an unknown value is reported"), PlayerJson->HasField(TEXT("value_errors")));
			TestTrue(TEXT("the pawn has its controller"), PlayerJson->HasField(TEXT("controller")));
		}
		const FLevelToolResult Tagged = Query(TEXT("{\"tag\": \"crate\"}"));
		const TSharedPtr<FJsonObject> CrateJson = FirstActor(Tagged);
		if (TestNotNull(TEXT("query by tag"), CrateJson.Get()))
		{
			TestEqual(TEXT("one tagged actor"), static_cast<int32>(Tagged.Payload->GetNumberField(TEXT("total"))), 1);
			TestEqual(TEXT("its label"), CrateJson->GetStringField(TEXT("label")), FString(TEXT("PlayTestCrate")));
			TestEqual(TEXT("its location"), CrateJson->GetObjectField(TEXT("location"))->GetNumberField(TEXT("X")), 500.0);
		}
		const FLevelToolResult ByName = Query(TEXT("{\"actors\": [\"PlayTestCrate\", \"NoSuchActor\"]}"));
		TestTrue(TEXT("an unknown actor is reported"), HasCode(ByName, TEXT("Play.ActorNotFound")));
		const FLevelToolResult Paged = Query(TEXT("{\"limit\": 1, \"offset\": 1}"));
		TestTrue(TEXT("paging"), Paged.bSuccess && Paged.Payload->GetArrayField(TEXT("actors")).Num() == 1
			&& Paged.Payload->GetNumberField(TEXT("total")) > 1);
		TestTrue(TEXT("game mode"), FirstActor(Query(TEXT("{\"actors\": [\"game_mode\"]}"))).IsValid());
	}));

	// Input validation inside a session.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"status\"}"), [this](const FLevelToolResult&)
	{
		TestTrue(TEXT("unknown action"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"input\", \"inputs\": [{\"input_action\": \"IA_NoSuchActionForPlayTests\"}]}")), TEXT("Play.InputActionNotFound")));
		TestTrue(TEXT("unknown key"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"NoSuchKey\"}]}")), TEXT("Play.UnknownKey")));
		TestTrue(TEXT("action and key"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"J\", \"input_action\": \"IA_X\"}]}")), TEXT("Play.InvalidRequest")));
		TestTrue(TEXT("no inputs"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"input\", \"inputs\": []}")), TEXT("Play.InvalidRequest")));
		TestTrue(TEXT("refused console command"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"console\", \"command\": \"quit\"}")), TEXT("Play.CommandRefused")));
		TestNotNull(TEXT("still playing"), GEditor->PlayWorld.Get());
	}));

	// An Input Action injected by path: the Blueprint's event runs.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(FString::Printf(TEXT("{\"action\": \"input\", \"inputs\": [{\"input_action\": \"%s\"}], \"settle\": 0.1}"),
		*State->ActionPath), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("input succeeds (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(TEXT("the action is not mapped yet"), HasCode(Result, TEXT("Play.ActionNotMapped")));
		const TSharedPtr<FJsonObject> Action = FirstInputAction(Result);
		if (TestNotNull(TEXT("the input reports its action"), Action.Get()))
		{
			TestTrue(TEXT("the action triggered"), Action->GetBoolField(TEXT("triggered")));
			TArray<FString> Events;
			Action->TryGetStringArrayField(TEXT("events"), Events);
			TestTrue(TEXT("a tap starts, triggers and completes"), Events.Contains(TEXT("Started")) && Events.Contains(TEXT("Triggered"))
				&& Events.Contains(TEXT("Completed")));
		}
		TestEqual(TEXT("input leaves the game paused"), SessionState(Result), FString(TEXT("paused")));
		const TArray<TSharedPtr<FJsonValue>>& Inputs = Result.Payload->GetArrayField(TEXT("inputs"));
		TestTrue(TEXT("the pawn listens to the action"), Inputs.Num() == 1 && Inputs[0]->AsObject()->GetArrayField(TEXT("listeners")).Num() > 0);
		const TSharedPtr<FJsonObject> PlayerJson = FirstActor(Query(TEXT("{\"actors\": [\"player\"], \"properties\": [\"bActionFired\"]}")));
		TestTrue(TEXT("the Blueprint set bActionFired"), PlayerJson.IsValid() && PlayerJson->GetObjectField(TEXT("values"))->GetBoolField(TEXT("bActionFired")));
		State->PrintsAfterAction = CountPrints();
		TestEqual(TEXT("the Print String is in the play log"), State->PrintsAfterAction, 1);
		const TSharedPtr<FJsonObject>* Log = nullptr;
		TestTrue(TEXT("the result shows the print"), Result.Payload->TryGetObjectField(TEXT("log"), Log) && (*Log)->HasField(TEXT("print")));

		// Map the action to J as a game's mapping context does.
		APlayerController* Controller = Play::GetLocalPlayerController(*GEditor->PlayWorld);
		UEnhancedInputLocalPlayerSubsystem* Subsystem = Controller != nullptr
			? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()) : nullptr;
		if (TestNotNull(TEXT("input subsystem"), Subsystem))
		{
			Subsystem->AddMappingContext(State->Context.Get(), 0);
		}
	}));

	// The key the context maps: the action triggers through the mapping (once the subsystem has rebuilt its
	// mappings for the added context, a frame later: keys held during that rebuild are ignored until released).
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"J\", \"duration\": 0.1, \"at\": 0.1}], \"settle\": 0.1}"),
		[this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("key input succeeds (%s)"), *Codes(Result)), Result.bSuccess);
		TestFalse(TEXT("J is mapped"), HasCode(Result, TEXT("Play.KeyNotMapped")));
		const TSharedPtr<FJsonObject> Action = FirstInputAction(Result);
		if (TestNotNull(TEXT("the key reports the mapped action"), Action.Get()))
		{
			TestEqual(TEXT("it is the test action"), Action->GetStringField(TEXT("action")), State->Action->GetName());
			TestTrue(TEXT("the mapped action triggered"), Action->GetBoolField(TEXT("triggered")));
		}
		const int32 Prints = CountPrints();
		if (!TestTrue(TEXT("the Blueprint printed again"), Prints > State->PrintsAfterAction))
		{
			FString Text;
			FJsonSerializer::Serialize(Result.Payload.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
			AddInfo(FString::Printf(TEXT("prints %d; key result: %s"), Prints, *Text.Left(3000)));
		}
	}));

	// Waits: a value condition already true, game time, a condition never met (bounded).
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(
		TEXT("{\"action\": \"wait\", \"until\": {\"actor\": \"player\", \"property\": \"bActionFired\", \"value\": true}}"),
		[this](const FLevelToolResult& Result)
	{
		TestTrue(TEXT("value condition met"), Result.bSuccess && Result.Payload->GetBoolField(TEXT("satisfied")));
		TestEqual(TEXT("reason"), Result.Payload->GetStringField(TEXT("reason")), FString(TEXT("condition")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"wait\", \"seconds\": 0.25}"), [this](const FLevelToolResult& Result)
	{
		TestTrue(TEXT("time wait satisfied"), Result.bSuccess && Result.Payload->GetBoolField(TEXT("satisfied")));
		TestTrue(TEXT("advanced game time"), Result.Payload->GetNumberField(TEXT("waited_game_seconds")) >= 0.25);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(
		TEXT("{\"action\": \"wait\", \"until\": {\"count\": {\"tag\": \"crate\"}, \"op\": \">=\", \"value\": 2}, \"timeout\": 1}"),
		[this](const FLevelToolResult& Result)
	{
		TestTrue(TEXT("a timed-out wait still answers"), Result.bSuccess);
		TestFalse(TEXT("not satisfied"), Result.Payload->GetBoolField(TEXT("satisfied")));
		TestTrue(TEXT("Play.WaitTimedOut"), HasCode(Result, TEXT("Play.WaitTimedOut")));
		TestTrue(TEXT("the last observed count"), Result.Payload->HasField(TEXT("last_observed")));
	}));

	// Pause: a wait runs the game and pauses it again.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"pause\"}"), [this](const FLevelToolResult& Result)
	{
		TestEqual(TEXT("paused"), SessionState(Result), FString(TEXT("paused")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"wait\", \"seconds\": 0.1}"), [this](const FLevelToolResult& Result)
	{
		TestTrue(TEXT("a wait advances a paused game"), Result.bSuccess && Result.Payload->GetBoolField(TEXT("satisfied")));
		TestEqual(TEXT("and pauses it again"), SessionState(Result), FString(TEXT("paused")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"resume\"}"), [this](const FLevelToolResult& Result)
	{
		TestEqual(TEXT("resumed"), SessionState(Result), FString(TEXT("playing")));
		const FLevelToolResult Console = Play::RunPlayFromBody(TEXT("{\"action\": \"console\", \"command\": \"slomo 1\"}"));
		TestTrue(*FString::Printf(TEXT("console (%s)"), *Codes(Console)), Console.bSuccess);
	}));

	// The game view, not the editor viewport.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"wait\", \"seconds\": 0.1}"), [this](const FLevelToolResult&)
	{
		const FLevelToolResult Shot = Play::RunPlayScreenshotFromBody(TEXT("{}"));
		if (TestTrue(*FString::Printf(TEXT("screenshot (%s)"), *Codes(Shot)), Shot.bSuccess && Shot.Payload.IsValid()))
		{
			TestEqual(TEXT("game view while playing"), Shot.Payload->GetStringField(TEXT("view")), FString(TEXT("game")));
			TestTrue(TEXT("has pixels"), Shot.Payload->GetNumberField(TEXT("width")) > 0 && Shot.Payload->GetStringField(TEXT("data")).Len() > 100);
			TestEqual(TEXT("JPEG"), Shot.Payload->GetStringField(TEXT("mime_type")), FString(TEXT("image/jpeg")));
			TestTrue(TEXT("at most 1024 px wide by default"), Shot.Payload->GetNumberField(TEXT("width")) <= 1024);
			TestEqual(TEXT("default quality"), static_cast<int32>(Shot.Payload->GetNumberField(TEXT("quality"))), 70);
			TestEqual(TEXT("bytes = the base64 length"), static_cast<int32>(Shot.Payload->GetNumberField(TEXT("bytes"))),
				Shot.Payload->GetStringField(TEXT("data")).Len());
		}
		// Sizes: the default against 1280 px quality 85, a lossless PNG and a small preview.
		const FLevelToolResult Large = Play::RunPlayScreenshotFromBody(TEXT("{\"quality\": 85, \"max_width\": 1280}"));
		const FLevelToolResult Png = Play::RunPlayScreenshotFromBody(TEXT("{\"format\": \"png\"}"));
		const FLevelToolResult Small = Play::RunPlayScreenshotFromBody(TEXT("{\"quality\": 40, \"max_width\": 320}"));
		if (TestTrue(*FString::Printf(TEXT("screenshot variants (%s %s %s)"), *Codes(Large), *Codes(Png), *Codes(Small)),
			Shot.bSuccess && Large.bSuccess && Png.bSuccess && Small.bSuccess))
		{
			const int32 DefaultBytes = Shot.Payload->GetStringField(TEXT("data")).Len();
			const int32 LargeBytes = Large.Payload->GetStringField(TEXT("data")).Len();
			const int32 PngBytes = Png.Payload->GetStringField(TEXT("data")).Len();
			const int32 SmallBytes = Small.Payload->GetStringField(TEXT("data")).Len();
			AddInfo(FString::Printf(TEXT("screenshot base64 sizes: %dx%d viewport; q85 1280 px %d, default (q70 1024 px) %d, PNG %d, q40 320 px %d"),
				static_cast<int32>(Large.Payload->GetNumberField(TEXT("width"))), static_cast<int32>(Large.Payload->GetNumberField(TEXT("height"))),
				LargeBytes, DefaultBytes, PngBytes, SmallBytes));
			TestEqual(TEXT("PNG"), Png.Payload->GetStringField(TEXT("mime_type")), FString(TEXT("image/png")));
			TestFalse(TEXT("a PNG has no quality"), Png.Payload->HasField(TEXT("quality")));
			TestTrue(TEXT("max_width"), Small.Payload->GetNumberField(TEXT("width")) <= 320);
			TestTrue(TEXT("the default is smaller than the large settings"), DefaultBytes < LargeBytes);
			TestTrue(TEXT("lossless is larger"), PngBytes > DefaultBytes);
			TestTrue(TEXT("a small preview is smaller"), SmallBytes < DefaultBytes);
		}
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"stop\"}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop (%s)"), *Codes(Result)), Result.bSuccess);
		TestEqual(TEXT("stopped"), SessionState(Result), FString(TEXT("stopped")));
		TestNull(TEXT("no play world"), GEditor->PlayWorld.Get());
		TestTrue(TEXT("refused after the session"), HasCode(Query(TEXT("{}")), TEXT("Play.NoSession")));
		TestTrue(TEXT("the session's log stays readable"), CountPrints() >= 2);
		UWorld* EditorWorld = GetEditorWorld();
		bool bEditorUntouched = false;
		for (TActorIterator<APawn> It(EditorWorld); It; ++It)
		{
			bEditorUntouched |= It->GetActorLabel() == TEXT("PlayTestPawn");
		}
		TestTrue(TEXT("the editor world keeps its pawn"), bEditorUntouched);
		DiscardBlueprint(State->Blueprint.Get());
		State->Blueprint.Reset();
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}));
	return true;
}

// The tester's debug tools. A paused start stops before the first frame and runs its console
// commands there; "ke <name>" reaches the game's actor, not the editor's copy, and returns its output; call runs
// a Blueprint function or UFUNCTION with struct arguments and returns its outputs; set writes values and
// teleports; input "down" / "up" hold and release across calls; query_play lists effect components and full
// paths; the session reports the real time it ran unpaused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayToolsTest,
	"UEAssetDoc.Level.Play.Tools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayToolsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPlayToolsTests;
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("editor world"), World))
	{
		return false;
	}
	TSharedRef<FSessionState> State = MakeShared<FSessionState>();
	State->Action.Reset(NewInputAction(TEXT("IA_PlayToolsHold")));
	State->ActionPath = State->Action->GetPathName();
	State->Context.Reset(NewObject<UInputMappingContext>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UInputMappingContext::StaticClass(), TEXT("IMC_PlayTools"))));
	State->Context->MapKey(State->Action.Get(), EKeys::J);
	State->Blueprint.Reset(NewPawnBlueprint(State->Action.Get(), false));
	AddEchoFunction(State->Blueprint.Get());
	if (!TestTrue(TEXT("the test Blueprint compiles"), State->Blueprint->IsUpToDate())
		|| !TestNotNull(TEXT("it has Echo"), State->Blueprint->GeneratedClass->FindFunctionByName(TEXT("Echo"))))
	{
		DiscardBlueprint(State->Blueprint.Get());
		return false;
	}
	APawn* Pawn = Cast<APawn>(SpawnActor(*World->PersistentLevel, State->Blueprint->GeneratedClass, FVector(0.0, 0.0, 300.0), TEXT("ToolsPawn")));
	AActor* Crate = SpawnActor(*World->PersistentLevel, AStaticMeshActor::StaticClass(), FVector(500.0, 0.0, 0.0), TEXT("ToolsCrate"));
	if (!TestNotNull(TEXT("pawn placed"), Pawn) || !TestNotNull(TEXT("crate placed"), Crate))
	{
		DiscardBlueprint(State->Blueprint.Get());
		return false;
	}
	Pawn->AutoPossessPlayer = EAutoReceiveInput::Player0;
	// The game's copy of a placed actor has the editor actor's object name: "ke <that name>" was ambiguous.
	const FString CrateName = Crate->GetName();
	const TWeakObjectPtr<AActor> EditorCrate = Crate;

	// Paused before the first frame; the start's console runs there.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(FString::Printf(TEXT("{\"action\": \"start\", \"paused\": true, \"console\": "
		"[\"ke %s SetActorHiddenInGame true\"]}"), *CrateName), [this, CrateName, EditorCrate](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("start succeeds (%s)"), *Codes(Result)), Result.bSuccess);
		const TSharedPtr<FJsonObject> SessionJson = Session(Result);
		if (TestNotNull(TEXT("session"), SessionJson.Get()))
		{
			TestEqual(TEXT("paused"), SessionJson->GetStringField(TEXT("state")), FString(TEXT("paused")));
			TestEqual(TEXT("paused before the first frame: world time 0"), SessionJson->GetNumberField(TEXT("world_time")), 0.0);
			TestTrue(TEXT("unpaused real time is reported"), SessionJson->HasField(TEXT("unpaused_real_time")));
		}
		const TArray<TSharedPtr<FJsonValue>>* Console = nullptr;
		if (TestTrue(TEXT("the start reports its console commands"), Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("console"), Console)
			&& Console->Num() == 1))
		{
			const TSharedPtr<FJsonObject> Ran = (*Console)[0]->AsObject();
			TestTrue(*FString::Printf(TEXT("the ke output is returned (%s)"), *Ran->GetStringField(TEXT("output"))),
				Ran->GetStringField(TEXT("output")).Contains(TEXT("succeeded")));
			TestTrue(TEXT("the target was resolved in the play world"), Ran->HasField(TEXT("resolved")));
		}
		AActor* PlayCrate = FindPlayActor(TEXT("ToolsCrate"));
		TestTrue(TEXT("the game's crate is hidden"), PlayCrate != nullptr && PlayCrate->IsHidden());
		TestTrue(TEXT("the editor's crate is not"), EditorCrate.IsValid() && !EditorCrate->IsHidden());
	}));

	// query_play: full play-world paths; detail components; ke from play console with output.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(FString::Printf(TEXT("{\"action\": \"console\", \"command\": \"ke %s SetActorHiddenInGame false\"}"),
		*CrateName), [this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("console (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(TEXT("console output is returned"), Result.Payload.IsValid() && Result.Payload->GetStringField(TEXT("output")).Contains(TEXT("UEDPIE")));
		AActor* PlayCrate = FindPlayActor(TEXT("ToolsCrate"));
		TestTrue(TEXT("the game's crate is visible again"), PlayCrate != nullptr && !PlayCrate->IsHidden());
		const TSharedPtr<FJsonObject> CrateJson = FirstActor(Query(TEXT("{\"actors\": [\"ToolsCrate\"], \"detail\": \"components\"}")));
		if (TestNotNull(TEXT("query the crate"), CrateJson.Get()))
		{
			TestTrue(TEXT("its full play-world path"), CrateJson->GetStringField(TEXT("path")).Contains(TEXT("UEDPIE")));
			TestTrue(TEXT("detail components lists components"), CrateJson->HasField(TEXT("components")));
			TestFalse(TEXT("and no properties"), CrateJson->HasField(TEXT("properties")));
			// The path names the actor again.
			TestTrue(TEXT("the path is an actor reference"), FirstActor(Query(FString::Printf(TEXT("{\"actors\": [\"%s\"]}"),
				*CrateJson->GetStringField(TEXT("path"))))).IsValid());
		}
	}));

	// call: a Blueprint function with a struct argument and output; a UFUNCTION with struct, return value and out
	// parameter; a component function; the refusals.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"Echo\", "
		"\"args\": {\"Value\": {\"X\": 1, \"Y\": 2, \"Z\": 3}}}"), [this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("call Echo (%s)"), *Codes(Result)), Result.bSuccess);
		const TSharedPtr<FJsonObject> Echoed = ObjectAt(ObjectAt(Result.Payload, TEXT("outputs")), TEXT("Result"));
		TestTrue(TEXT("the Blueprint function's output"), Echoed.IsValid() && Echoed->GetNumberField(TEXT("Y")) == 2.0 && Echoed->GetNumberField(TEXT("Z")) == 3.0);
		TestTrue(TEXT("the session comes with the result"), Session(Result).IsValid());

		const FLevelToolResult Moved = Play::RunPlayFromBody(TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"K2_SetActorLocation\", "
			"\"args\": {\"NewLocation\": {\"X\": 100, \"Y\": 200, \"Z\": 300}, \"bTeleport\": true}}"));
		TestTrue(*FString::Printf(TEXT("call K2_SetActorLocation (%s)"), *Codes(Moved)), Moved.bSuccess);
		TestTrue(TEXT("its return value"), Moved.Payload.IsValid() && Moved.Payload->HasTypedField<EJson::Boolean>(TEXT("return"))
			&& Moved.Payload->GetBoolField(TEXT("return")));
		TestTrue(TEXT("its out parameter"), ObjectAt(Moved.Payload, TEXT("outputs")).IsValid() && ObjectAt(Moved.Payload, TEXT("outputs"))->HasField(TEXT("SweepHitResult")));
		TArray<FString> Defaulted;
		TestTrue(TEXT("parameters left out take the node's default"), Moved.Payload.IsValid() && Moved.Payload->TryGetStringArrayField(TEXT("defaulted"), Defaulted)
			&& Defaulted.Contains(TEXT("bSweep")));
		AActor* PlayPawn = FindPlayActor(TEXT("ToolsPawn"));
		TestTrue(TEXT("the game's pawn moved"), PlayPawn != nullptr && PlayPawn->GetActorLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));

		const FLevelToolResult Positional = Play::RunPlayFromBody(TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"Echo\", \"args\": [{\"X\": 7}]}"));
		const TSharedPtr<FJsonObject> PositionalOut = ObjectAt(ObjectAt(Positional.Payload, TEXT("outputs")), TEXT("Result"));
		TestTrue(TEXT("positional arguments"), PositionalOut.IsValid() && PositionalOut->GetNumberField(TEXT("X")) == 7.0);

		if (PlayPawn != nullptr && PlayPawn->GetRootComponent() != nullptr)
		{
			const FLevelToolResult OnComponent = Play::RunPlayFromBody(FString::Printf(TEXT("{\"action\": \"call\", \"actor\": \"player\", "
				"\"function\": \"%s.K2_GetComponentLocation\"}"), *PlayPawn->GetRootComponent()->GetName()));
			const TSharedPtr<FJsonObject> Location = ObjectAt(OnComponent.Payload, TEXT("return"));
			TestTrue(*FString::Printf(TEXT("a component function (%s)"), *Codes(OnComponent)), Location.IsValid() && Location->GetNumberField(TEXT("X")) == 100.0);
		}
		TestTrue(TEXT("unknown function"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"Ecko\"}")), TEXT("Play.FunctionNotFound")));
		const FLevelToolResult Close = Play::RunPlayFromBody(TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"Ecko\"}"));
		TestTrue(TEXT("the close names include Echo"), Close.Diagnostics.Num() > 0 && Close.Diagnostics[0].Message.Contains(TEXT("Echo")));
		TestTrue(TEXT("unknown parameter"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"Echo\", \"args\": {\"Valeu\": 1}}")), TEXT("Play.InvalidRequest")));
		TestTrue(TEXT("a value of the wrong type"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"Echo\", \"args\": {\"Value\": \"north\"}}")), TEXT("Play.InvalidArgument")));
		TestTrue(TEXT("unknown component"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"call\", \"actor\": \"player\", \"function\": \"NoSuchComponent.Activate\"}")), TEXT("Play.ComponentNotFound")));
		TestTrue(TEXT("unknown actor"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"call\", \"actor\": \"NoSuchActor\", \"function\": \"Echo\"}")), TEXT("Play.ActorNotFound")));
		if (PlayPawn != nullptr && PlayPawn->GetClass()->ClassGeneratedBy != nullptr)
		{
			TestTrue(TEXT("the internal event graph"), HasCode(Play::RunPlayFromBody(FString::Printf(TEXT("{\"action\": \"call\", \"actor\": \"player\", "
				"\"function\": \"ExecuteUbergraph_%s\"}"), *PlayPawn->GetClass()->ClassGeneratedBy->GetName())), TEXT("Play.FunctionNotCallable")));
		}
	}));

	// set: values and a teleport; checked before anything is written.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"set\", \"actor\": \"player\", \"values\": {\"bActionFired\": true}, "
		"\"location\": {\"Z\": 500}}"), [this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("set (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(TEXT("two changes"), Result.Payload.IsValid() && Result.Payload->GetArrayField(TEXT("changes")).Num() == 2);
		const TSharedPtr<FJsonObject> PlayerJson = FirstActor(Query(TEXT("{\"actors\": [\"player\"], \"properties\": [\"bActionFired\"]}")));
		if (TestNotNull(TEXT("query the player"), PlayerJson.Get()))
		{
			TestTrue(TEXT("the value was written"), PlayerJson->GetObjectField(TEXT("values"))->GetBoolField(TEXT("bActionFired")));
			TestEqual(TEXT("teleported (Z only)"), PlayerJson->GetObjectField(TEXT("location"))->GetNumberField(TEXT("Z")), 500.0);
			TestEqual(TEXT("X kept"), PlayerJson->GetObjectField(TEXT("location"))->GetNumberField(TEXT("X")), 100.0);
		}
		const FLevelToolResult Refused = Play::RunPlayFromBody(TEXT("{\"action\": \"set\", \"actor\": \"player\", \"values\": {\"bActionFired\": false, \"NoSuchValue\": 1}}"));
		TestTrue(TEXT("an unknown path"), HasCode(Refused, TEXT("Play.PropertyNotFound")));
		TestTrue(TEXT("a wrong value"), HasCode(Play::RunPlayFromBody(
			TEXT("{\"action\": \"set\", \"actor\": \"player\", \"values\": {\"bActionFired\": {\"X\": 1}}}")), TEXT("Play.InvalidValue")));
		const TSharedPtr<FJsonObject> After = FirstActor(Query(TEXT("{\"actors\": [\"player\"], \"properties\": [\"bActionFired\"]}")));
		TestTrue(TEXT("nothing was written by the refused sets"), After.IsValid() && After->GetObjectField(TEXT("values"))->GetBoolField(TEXT("bActionFired")));
	}));

	// Effects: a Cascade component the game adds to the crate.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"status\"}"), [this, State](const FLevelToolResult&)
	{
		AActor* PlayCrate = FindPlayActor(TEXT("ToolsCrate"));
		if (!TestNotNull(TEXT("the game's crate"), PlayCrate))
		{
			return;
		}
		UParticleSystem* System = NewObject<UParticleSystem>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UParticleSystem::StaticClass(), TEXT("PS_PlayToolsEffect")));
		UParticleSystemComponent* Effect = NewObject<UParticleSystemComponent>(PlayCrate,
			MakeUniqueObjectName(PlayCrate, UParticleSystemComponent::StaticClass(), TEXT("ToolsEffect")));
		Effect->SetTemplate(System);
		Effect->SetupAttachment(PlayCrate->GetRootComponent());
		Effect->RegisterComponent();
		Effect->Activate(true);
		const FLevelToolResult All = Query(TEXT("{\"what\": \"effects\", \"include_inactive\": true}"));
		TSharedPtr<FJsonObject> Listed;
		if (TestTrue(*FString::Printf(TEXT("effects query (%s)"), *Codes(All)), All.bSuccess))
		{
			for (const TSharedPtr<FJsonValue>& Entry : All.Payload->GetArrayField(TEXT("effects")))
			{
				if (Entry->AsObject()->GetStringField(TEXT("asset")) == System->GetPathName())
				{
					Listed = Entry->AsObject();
				}
			}
		}
		if (TestNotNull(TEXT("the effect component is listed with its asset"), Listed.Get()))
		{
			TestEqual(TEXT("Cascade"), Listed->GetStringField(TEXT("type")), FString(TEXT("Cascade")));
			TestEqual(TEXT("its owner"), Listed->GetStringField(TEXT("owner")), PlayCrate->GetName());
			TestEqual(TEXT("its active state"), Listed->GetBoolField(TEXT("active")), Effect->IsActive());
			TestTrue(TEXT("its location"), Listed->HasField(TEXT("location")));
		}
		const FLevelToolResult Near = Query(TEXT("{\"what\": \"effects\", \"include_inactive\": true, \"near\": {\"actor\": \"ToolsCrate\", \"radius\": 10}}"));
		TestTrue(TEXT("near the crate"), Near.bSuccess && Near.Payload->GetNumberField(TEXT("total")) >= 1);
		TestTrue(TEXT("an actor field with what=effects is refused"), HasCode(Query(TEXT("{\"what\": \"effects\", \"class\": \"Pawn\"}")), TEXT("Play.InvalidRequest")));
		Effect->DestroyComponent();

		// Map J for the hold tests below.
		APlayerController* Controller = Play::GetLocalPlayerController(*GEditor->PlayWorld);
		UEnhancedInputLocalPlayerSubsystem* Subsystem = Controller != nullptr
			? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()) : nullptr;
		if (TestNotNull(TEXT("input subsystem"), Subsystem))
		{
			Subsystem->AddMappingContext(State->Context.Get(), 0);
		}
	}));

	// Hold J across calls: down, the game runs (wait), up.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"J\", \"state\": \"down\", \"at\": 0.1}], \"settle\": 0.1}"),
		[this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("down (%s)"), *Codes(Result)), Result.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		TestTrue(TEXT("reported held"), Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("inputs"), Inputs)
			&& (*Inputs)[0]->AsObject()->HasTypedField<EJson::Boolean>(TEXT("held")));
		const TSharedPtr<FJsonObject> Action = FirstInputAction(Result);
		TestTrue(TEXT("the mapped action triggered"), Action.IsValid() && Action->GetBoolField(TEXT("triggered")));
		TestFalse(TEXT("not completed while held"), InputEvents(Result).Contains(TEXT("Completed")));
		TArray<FString> Held;
		TestTrue(TEXT("the session lists the held key"), Session(Result).IsValid() && Session(Result)->TryGetStringArrayField(TEXT("held_inputs"), Held)
			&& Held.Contains(TEXT("J")));
		TestTrue(TEXT("a tap of a held key is refused"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"J\"}]}")),
			TEXT("Play.InputHeld")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"wait\", \"seconds\": 0.2}"), [this](const FLevelToolResult& Result)
	{
		TArray<FString> Held;
		TestTrue(TEXT("still held after a wait"), Session(Result).IsValid() && Session(Result)->TryGetStringArrayField(TEXT("held_inputs"), Held)
			&& Held.Contains(TEXT("J")));
		TestTrue(TEXT("the session ran unpaused for a while"), Session(Result).IsValid() && Session(Result)->GetNumberField(TEXT("unpaused_real_time")) > 0.0
			&& Session(Result)->GetNumberField(TEXT("unpaused_real_time")) <= Session(Result)->GetNumberField(TEXT("real_time")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"J\", \"state\": \"up\"}]}"),
		[this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("up (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(*FString::Printf(TEXT("released: Completed (%s)"), *FString::Join(InputEvents(Result), TEXT(","))), InputEvents(Result).Contains(TEXT("Completed")));
		TestFalse(TEXT("nothing held"), Session(Result).IsValid() && Session(Result)->HasField(TEXT("held_inputs")));
		TestTrue(TEXT("up of a key not held"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"J\", \"state\": \"up\"}]}")),
			TEXT("Play.InputNotHeld")));
		TestTrue(TEXT("unknown state"), HasCode(Play::RunPlayFromBody(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"J\", \"state\": \"hold\"}]}")),
			TEXT("Play.InvalidRequest")));
	}));
	// An Input Action held and released the same way.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(FString::Printf(TEXT("{\"action\": \"input\", \"inputs\": [{\"input_action\": \"%s\", \"state\": \"down\"}]}"),
		*State->ActionPath), [this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("action down (%s)"), *Codes(Result)), Result.bSuccess);
		const TSharedPtr<FJsonObject> Action = FirstInputAction(Result);
		TestTrue(TEXT("the held action triggers"), Action.IsValid() && Action->GetBoolField(TEXT("triggered")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(FString::Printf(TEXT("{\"action\": \"input\", \"inputs\": [{\"input_action\": \"%s\", \"state\": \"up\"}]}"),
		*State->ActionPath), [this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("action up (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(*FString::Printf(TEXT("action released: Completed (%s)"), *FString::Join(InputEvents(Result), TEXT(","))),
			InputEvents(Result).Contains(TEXT("Completed")));
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"stop\"}"), [this, State, EditorCrate](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop (%s)"), *Codes(Result)), Result.bSuccess);
		TestTrue(TEXT("the unpaused real time of the session"), Result.Payload.IsValid() && Result.Payload->HasField(TEXT("played_unpaused_real_seconds")));
		TestTrue(TEXT("the editor's crate never moved or hid"), EditorCrate.IsValid() && !EditorCrate->IsHidden()
			&& EditorCrate->GetActorLocation().Equals(FVector(500.0, 0.0, 0.0)));
		DiscardBlueprint(State->Blueprint.Get());
		State->Blueprint.Reset();
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}));
	return true;
}
namespace AssetDocumentPlayToolsTests
{
// An Actor Blueprint whose custom event "Chime" plays Ping at (100, 200, 300) (Play Sound at Location: no component)
// and spawns Noise at its own location (Spawn Sound at Location: an audio component).
UBlueprint* NewChimeBlueprint(USoundBase* Ping, USoundBase* Noise)
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BP_PlayAudioTest")), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
	UK2Node_CustomEvent* Event = EventCreator.CreateNode(false);
	Event->CustomFunctionName = TEXT("Chime");
	EventCreator.Finalize();

	FGraphNodeCreator<UK2Node_CallFunction> PlayCreator(*Graph);
	UK2Node_CallFunction* PlaySound = PlayCreator.CreateNode(false);
	PlaySound->SetFromFunction(UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("PlaySoundAtLocation")));
	PlayCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> SpawnCreator(*Graph);
	UK2Node_CallFunction* SpawnSound = SpawnCreator.CreateNode(false);
	SpawnSound->SetFromFunction(UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("SpawnSoundAtLocation")));
	SpawnCreator.Finalize();
	if (UEdGraphPin* Sound = PlaySound->FindPin(TEXT("Sound")))
	{
		Sound->DefaultObject = Ping;
	}
	if (UEdGraphPin* Location = PlaySound->FindPin(TEXT("Location")))
	{
		Location->DefaultValue = TEXT("100.000000,200.000000,300.000000");
	}
	if (UEdGraphPin* Sound = SpawnSound->FindPin(TEXT("Sound")))
	{
		Sound->DefaultObject = Noise;
	}
	Event->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(PlaySound->GetExecPin());
	PlaySound->GetThenPin()->MakeLinkTo(SpawnSound->GetExecPin());
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	return Blueprint;
}

TArray<TSharedPtr<FJsonObject>> AudioEntries(const FLevelToolResult& Result, const TCHAR* Field)
{
	TArray<TSharedPtr<FJsonObject>> Out;
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (Result.bSuccess && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(Field, Entries) && Entries != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *Entries)
		{
			Out.Add(Entry->AsObject());
		}
	}
	return Out;
}

TSharedPtr<FJsonObject> SoundNamed(const FLevelToolResult& Result, const FString& Sound)
{
	for (const TSharedPtr<FJsonObject>& Entry : AudioEntries(Result, TEXT("sounds")))
	{
		if (Entry->GetStringField(TEXT("sound")) == Sound)
		{
			return Entry;
		}
	}
	return nullptr;
}

struct FAudioState
{
	TStrongObjectPtr<UBlueprint> Blueprint;
	TStrongObjectPtr<USoundBase> Ping;
	TStrongObjectPtr<USoundBase> Noise;
	FString ChimeActor;
	double SeqBeforeChime = 0.0;
};
} // namespace AssetDocumentPlayToolsTests

// query_play what "audio" verifies a sound in PIE. A Blueprint plays a sound at a location and spawns
// one with a component; the read lists both (asset, owner, location, component), then finished, and nothing since a
// later seq. Automation runs the editor with -nosound (no audio device): then the read answers Play.AudioDisabled
// with nothing listed, which this test checks instead (run it in an editor with audio to cover the recording).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayAudioTest,
	"UEAssetDoc.Level.Play.Audio",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayAudioTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPlayToolsTests;
	TestTrue(TEXT("audio needs a session"), HasCode(Query(TEXT("{\"what\": \"audio\"}")), TEXT("Play.NoSession")));
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("editor world"), World))
	{
		return false;
	}
	TSharedRef<FAudioState> State = MakeShared<FAudioState>();
	State->Ping.Reset(LoadObject<USoundBase>(nullptr, TEXT("/Engine/EngineSounds/1kSineTonePing.1kSineTonePing")));
	State->Noise.Reset(LoadObject<USoundBase>(nullptr, TEXT("/Engine/EngineSounds/WhiteNoise.WhiteNoise")));
	if (!TestNotNull(TEXT("engine sound 1kSineTonePing"), State->Ping.Get()) || !TestNotNull(TEXT("engine sound WhiteNoise"), State->Noise.Get()))
	{
		return false;
	}
	State->Blueprint.Reset(NewChimeBlueprint(State->Ping.Get(), State->Noise.Get()));
	if (!TestTrue(TEXT("the chime Blueprint compiles"), State->Blueprint->IsUpToDate()))
	{
		DiscardBlueprint(State->Blueprint.Get());
		return false;
	}
	if (!TestNotNull(TEXT("chime placed"), SpawnActor(*World->PersistentLevel, State->Blueprint->GeneratedClass, FVector(-300.0, 0.0, 100.0), TEXT("PlayAudioChime"))))
	{
		DiscardBlueprint(State->Blueprint.Get());
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"start\", \"paused\": true}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("start (%s)"), *Codes(Result)), Result.bSuccess);
		const TSharedPtr<FJsonObject> Chime = FirstActor(Query(TEXT("{\"actors\": [\"PlayAudioChime\"]}")));
		if (TestNotNull(TEXT("the chime actor plays"), Chime.Get()))
		{
			State->ChimeActor = Chime->GetStringField(TEXT("name"));
		}
		TestTrue(TEXT("a log field is refused for audio"), HasCode(Query(TEXT("{\"what\": \"audio\", \"category\": \"LogAudio\"}")), TEXT("Play.InvalidRequest")));
		TestTrue(TEXT("a bad since"), HasCode(Query(TEXT("{\"what\": \"audio\", \"since\": \"later\"}")), TEXT("Play.InvalidRequest")));
		TestTrue(TEXT("a bad near"), HasCode(Query(TEXT("{\"what\": \"audio\", \"near\": {\"radius\": -1}}")), TEXT("Play.InvalidRequest")));
		const FLevelToolResult Log = Query(TEXT("{\"what\": \"log\", \"limit\": 1}"));
		State->SeqBeforeChime = Log.Payload.IsValid() ? Log.Payload->GetNumberField(TEXT("last_seq")) : 0.0;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"call\", \"actor\": \"PlayAudioChime\", \"function\": \"Chime\"}"),
		[this](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("call Chime (%s)"), *Codes(Result)), Result.bSuccess);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"wait\", \"seconds\": 0.05}"), [this, State](const FLevelToolResult&)
	{
		const FLevelToolResult Audio = Query(TEXT("{\"what\": \"audio\"}"));
		if (!TestTrue(*FString::Printf(TEXT("query audio (%s)"), *Codes(Audio)), Audio.bSuccess))
		{
			return;
		}
		if (HasCode(Audio, TEXT("Play.AudioDisabled")))
		{
			AddInfo(TEXT("The editor runs without audio (-nosound): checked Play.AudioDisabled; run with audio to check the recording."));
			TestEqual(TEXT("no sound listed without audio"), AudioEntries(Audio, TEXT("sounds")).Num(), 0);
			TestEqual(TEXT("no component listed without audio"), AudioEntries(Audio, TEXT("components")).Num(), 0);
			return;
		}
		const TSharedPtr<FJsonObject> Ping = SoundNamed(Audio, State->Ping->GetPathName());
		if (TestNotNull(TEXT("Play Sound at Location is listed"), Ping.Get()))
		{
			TestEqual(TEXT("its owner is the Blueprint actor"), Ping->GetStringField(TEXT("owner")), State->ChimeActor);
			const TSharedPtr<FJsonObject> Location = ObjectAt(Ping, TEXT("location"));
			TestTrue(TEXT("at the location it was played"), Location.IsValid() && FMath::IsNearlyEqual(Location->GetNumberField(TEXT("X")), 100.0)
				&& FMath::IsNearlyEqual(Location->GetNumberField(TEXT("Y")), 200.0) && FMath::IsNearlyEqual(Location->GetNumberField(TEXT("Z")), 300.0));
			TestFalse(TEXT("no component"), Ping->HasField(TEXT("component")));
			TestTrue(TEXT("after the call"), Ping->GetNumberField(TEXT("seq")) > State->SeqBeforeChime);
		}
		const TSharedPtr<FJsonObject> Noise = SoundNamed(Audio, State->Noise->GetPathName());
		if (TestNotNull(TEXT("Spawn Sound at Location is listed"), Noise.Get()))
		{
			TestTrue(TEXT("with its audio component"), Noise->HasField(TEXT("component")));
			if (!Noise->GetBoolField(TEXT("finished")))
			{
				bool bListed = false;
				for (const TSharedPtr<FJsonObject>& Component : AudioEntries(Audio, TEXT("components")))
				{
					bListed |= Component->GetStringField(TEXT("sound")) == State->Noise->GetPathName();
				}
				TestTrue(TEXT("its playing component is listed"), bListed);
			}
		}
		const FLevelToolResult ByName = Query(TEXT("{\"what\": \"audio\", \"name\": \"1kSine*\"}"));
		TestEqual(TEXT("a name filter"), AudioEntries(ByName, TEXT("sounds")).Num(), 1);
		const FLevelToolResult Far = Query(TEXT("{\"what\": \"audio\", \"near\": {\"location\": {\"X\": 100, \"Y\": 200, \"Z\": 300}, \"radius\": 10}}"));
		TestEqual(TEXT("a near filter"), AudioEntries(Far, TEXT("sounds")).Num(), 1);
	}));
	// Both sounds are under half a second long.
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"wait\", \"seconds\": 1.5}"), [this, State](const FLevelToolResult& Result)
	{
		const FLevelToolResult Audio = Query(FString::Printf(TEXT("{\"what\": \"audio\", \"since\": %.0f}"), State->SeqBeforeChime));
		if (HasCode(Audio, TEXT("Play.AudioDisabled")))
		{
			return;
		}
		const TSharedPtr<FJsonObject> Ping = SoundNamed(Audio, State->Ping->GetPathName());
		if (TestNotNull(TEXT("the sound is still listed"), Ping.Get()))
		{
			TestTrue(TEXT("finished"), Ping->GetBoolField(TEXT("finished")));
			TestTrue(TEXT("finished after it started"), Ping->GetNumberField(TEXT("finished_t")) >= Ping->GetNumberField(TEXT("t")));
		}
		TestEqual(TEXT("no audio component plays any more"), AudioEntries(Audio, TEXT("components")).Num(), 0);
		const TSharedPtr<FJsonObject> Log = ObjectAt(Result.Payload, TEXT("log"));
		const FLevelToolResult Later = Query(FString::Printf(TEXT("{\"what\": \"audio\", \"since\": %.0f}"),
			Log.IsValid() ? Log->GetNumberField(TEXT("last_seq")) : 0.0));
		TestEqual(TEXT("nothing started since a later seq"), AudioEntries(Later, TEXT("sounds")).Num(), 0);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"stop\"}"), [this, State](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop (%s)"), *Codes(Result)), Result.bSuccess);
		DiscardBlueprint(State->Blueprint.Get());
		State->Blueprint.Reset();
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}));
	return true;
}
namespace AssetDocumentPlayToolsTests
{
// An Actor Blueprint with an object variable "Held" (an object reference a game could hold, or a document could set),
// a boolean "bTouched" and a function "ConsoleCommand(Command: String)" (named like the controller's console entry).
UBlueprint* NewHolderBlueprint()
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BP_PlayProtectedHolder")), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	FEdGraphPinType ObjectType;
	ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
	ObjectType.PinSubCategoryObject = UObject::StaticClass();
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Held"), ObjectType);
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bTouched"), BoolType);
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("ConsoleCommand"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);
	TArray<UK2Node_FunctionEntry*> Entries;
	Graph->GetNodesOfClass(Entries);
	if (!Entries.IsEmpty())
	{
		FEdGraphPinType StringType;
		StringType.PinCategory = UEdGraphSchema_K2::PC_String;
		Entries[0]->CreateUserDefinedPin(TEXT("Command"), StringType, EGPD_Output);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	return Blueprint;
}

struct FProtectedSettingsState
{
	TStrongObjectPtr<UBlueprint> Blueprint;
	bool bExecuteBefore = true;
	bool bDeleteBefore = true;
};
} // namespace AssetDocumentPlayToolsTests

// The play tools cannot change the plugin's own server and security settings: a console command that names them
// ("set <object> <property> <value>", at start or in the session), a function argument that does (ServerExec runs a
// console command), an object value that is their settings object, and a set through an object variable that holds it
// are refused, and execute_python and delete_assets stay switched off. Functions that run console commands are not
// called whatever their arguments (ServerExec with a class name built in Python, a function named ConsoleCommand,
// an exec function taking text), and a key whose debug key binding runs Python is not pressed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayProtectedSettingsTest,
	"UEAssetDoc.Level.Play.ProtectedSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayProtectedSettingsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPlayToolsTests;
	UAssetDocumentSettings* PluginSettings = UAssetDocumentSettings::Get();
	if (!TestNotNull(TEXT("the plugin settings"), PluginSettings))
	{
		return false;
	}
	TSharedRef<FProtectedSettingsState> State = MakeShared<FProtectedSettingsState>();
	State->bExecuteBefore = PluginSettings->bEnableExecutePython;
	State->bDeleteBefore = PluginSettings->bEnableDeleteAssets;
	PluginSettings->bEnableExecutePython = false;
	PluginSettings->bEnableDeleteAssets = false;
	const auto StillOff = [this, PluginSettings](const TCHAR* Stage)
	{
		TestFalse(*FString::Printf(TEXT("%s: execute_python stays off"), Stage), PluginSettings->bEnableExecutePython);
		TestFalse(*FString::Printf(TEXT("%s: delete_assets stays off"), Stage), PluginSettings->bEnableDeleteAssets);
	};

	// Checked before a session is needed.
	const FLevelToolResult ConsoleRefused = Play::RunPlayFromBody(
		TEXT("{\"action\": \"console\", \"command\": \"setnopec AssetDocumentSettings bEnableDeleteAssets 1\"}"));
	TestTrue(*FString::Printf(TEXT("a console command naming the settings is refused (%s)"), *Codes(ConsoleRefused)),
		HasCode(ConsoleRefused, TEXT("Play.CommandRefused")));
	StillOff(TEXT("before play"));

	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("editor world"), World))
	{
		PluginSettings->bEnableExecutePython = State->bExecuteBefore;
		PluginSettings->bEnableDeleteAssets = State->bDeleteBefore;
		return false;
	}
	State->Blueprint.Reset(NewHolderBlueprint());
	AActor* Holder = State->Blueprint->IsUpToDate()
		? SpawnActor(*World->PersistentLevel, State->Blueprint->GeneratedClass, FVector(0.0, 0.0, 0.0), TEXT("ProtectedHolder")) : nullptr;
	if (!TestNotNull(TEXT("the holder is placed"), Holder))
	{
		DiscardBlueprint(State->Blueprint.Get());
		PluginSettings->bEnableExecutePython = State->bExecuteBefore;
		PluginSettings->bEnableDeleteAssets = State->bDeleteBefore;
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"start\", \"paused\": true}"), [this, PluginSettings, StillOff](const FLevelToolResult& Result)
	{
		if (!TestTrue(*FString::Printf(TEXT("start succeeds (%s)"), *Codes(Result)), Result.bSuccess))
		{
			return;
		}
		for (const TCHAR* Command : {TEXT("set Default__AssetDocumentSettings bEnableExecutePython True"),
			TEXT("set /Script/AssetDocumentServer.Default__AssetDocumentSettings bEnableDeleteAssets True"),
			TEXT("slomo 1 | SET \"assetdocumentsettings\" bEnableExecutePython 1")})
		{
			const FLevelToolResult Refused = Play::RunPlayFromBody(FString::Printf(TEXT("{\"action\": \"console\", \"command\": \"%s\"}"),
				*FString(Command).ReplaceCharWithEscapedChar()));
			TestTrue(*FString::Printf(TEXT("'%s' is refused (%s)"), Command, *Codes(Refused)), HasCode(Refused, TEXT("Play.CommandRefused")));
		}
		StillOff(TEXT("console"));

		// ServerExec (an exec function of the player controller) runs a console command.
		const FLevelToolResult Exec = Play::RunPlayFromBody(TEXT("{\"action\": \"call\", \"actor\": \"controller\", \"function\": \"ServerExec\", "
			"\"args\": {\"Msg\": \"set Default__AssetDocumentSettings bEnableExecutePython True\"}}"));
		TestTrue(*FString::Printf(TEXT("a call whose argument names the settings is refused (%s)"), *Codes(Exec)),
			HasCode(Exec, TEXT("Play.ProtectedSettings")));
		TestTrue(*FString::Printf(TEXT("ServerExec is not callable (%s)"), *Codes(Exec)), HasCode(Exec, TEXT("Play.FunctionNotCallable")));
		// The class name built inside the command: only the function itself can be refused.
		const FLevelToolResult Built = Play::RunPlayFromBody(TEXT("{\"action\": \"call\", \"actor\": \"controller\", \"function\": \"ServerExec\", "
			"\"args\": {\"Msg\": \"py import unreal as u; u.load_class(None, '/Script/AssetDocumentServer.Asset' + 'DocumentSettings')\"}}"));
		TestTrue(*FString::Printf(TEXT("ServerExec with a built class name is refused (%s)"), *Codes(Built)),
			HasCode(Built, TEXT("Play.FunctionNotCallable")));
		for (const TCHAR* Body : {
			TEXT("{\"action\": \"call\", \"actor\": \"controller\", \"function\": \"SendToConsole\", \"args\": [\"slomo 1\"]}"),
			TEXT("{\"action\": \"call\", \"actor\": \"controller\", \"function\": \"SetName\", \"args\": [\"Tester\"]}"),
			TEXT("{\"action\": \"call\", \"actor\": \"ProtectedHolder\", \"function\": \"ConsoleCommand\", \"args\": [\"slomo 1\"]}")})
		{
			const FLevelToolResult Runner = Play::RunPlayFromBody(Body);
			TestTrue(*FString::Printf(TEXT("%s is refused (%s)"), Body, *Codes(Runner)), HasCode(Runner, TEXT("Play.FunctionNotCallable")));
		}
		StillOff(TEXT("call"));

		// A debug key binding that runs Python: the key is not pressed.
		APlayerController* PlayController = GEditor->PlayWorld != nullptr ? GEditor->PlayWorld->GetFirstPlayerController() : nullptr;
		if (TestTrue(TEXT("the play controller has a player input"), PlayController != nullptr && PlayController->PlayerInput != nullptr))
		{
			FKeyBind Bind;
			Bind.Key = EKeys::F9;
			Bind.Command = TEXT("slomo 1 | py import unreal");
			PlayController->PlayerInput->DebugExecBindings.Add(Bind);
			const FLevelToolResult Pressed = Play::RunPlayFromBody(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"F9\"}]}"));
			PlayController->PlayerInput->DebugExecBindings.RemoveAll([](const FKeyBind& Entry) { return Entry.Key == EKeys::F9; });
			TestTrue(*FString::Printf(TEXT("a key bound to Python is not pressed (%s)"), *Codes(Pressed)), HasCode(Pressed, TEXT("Play.CommandRefused")));
		}

		AActor* PlayHolder = FindPlayActor(TEXT("ProtectedHolder"));
		FObjectPropertyBase* Held = PlayHolder != nullptr ? CastField<FObjectPropertyBase>(PlayHolder->GetClass()->FindPropertyByName(TEXT("Held"))) : nullptr;
		if (!TestNotNull(TEXT("the play holder and its Held variable"), Held))
		{
			return;
		}
		const FLevelToolResult Store = Play::RunPlayFromBody(TEXT("{\"action\": \"set\", \"actor\": \"ProtectedHolder\", \"values\": "
			"{\"Held\": \"/Script/AssetDocumentServer.Default__AssetDocumentSettings\"}}"));
		TestTrue(*FString::Printf(TEXT("the settings object is not stored in a variable (%s)"), *Codes(Store)),
			HasCode(Store, TEXT("Play.ProtectedSettings")));
		TestNull(TEXT("Held stays empty"), Held->GetObjectPropertyValue_InContainer(PlayHolder));

		// A variable that already holds the settings object (as an authored default could): its values are not set through it.
		Held->SetObjectPropertyValue_InContainer(PlayHolder, PluginSettings);
		const FLevelToolResult Through = Play::RunPlayFromBody(TEXT("{\"action\": \"set\", \"actor\": \"ProtectedHolder\", \"values\": "
			"{\"Held.bEnableExecutePython\": true, \"Held.bEnableDeleteAssets\": true}}"));
		TestTrue(*FString::Printf(TEXT("a set through the variable is refused (%s)"), *Codes(Through)), HasCode(Through, TEXT("Play.ProtectedSettings")));
		Held->SetObjectPropertyValue_InContainer(PlayHolder, nullptr);
		StillOff(TEXT("set"));

		// Ordinary values and commands still work.
		const FLevelToolResult Touched = Play::RunPlayFromBody(TEXT("{\"action\": \"set\", \"actor\": \"ProtectedHolder\", \"values\": {\"bTouched\": true}}"));
		TestTrue(*FString::Printf(TEXT("an ordinary set works (%s)"), *Codes(Touched)), Touched.bSuccess);
		const FLevelToolResult Slomo = Play::RunPlayFromBody(TEXT("{\"action\": \"console\", \"command\": \"slomo 1\"}"));
		TestTrue(*FString::Printf(TEXT("an ordinary console command works (%s)"), *Codes(Slomo)), Slomo.bSuccess);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"stop\"}"), [this, State, PluginSettings, StillOff](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop (%s)"), *Codes(Result)), Result.bSuccess);
		StillOff(TEXT("after play"));
		// A start whose console commands name the settings is refused before anything starts.
		const FLevelToolResult StartRefused = Play::RunPlayFromBody(
			TEXT("{\"action\": \"start\", \"console\": [\"slomo 1\", \"set Default__AssetDocumentSettings bEnableExecutePython True\"]}"));
		TestTrue(*FString::Printf(TEXT("a start console command naming the settings is refused (%s)"), *Codes(StartRefused)),
			HasCode(StartRefused, TEXT("Play.CommandRefused")));
		if (!StartRefused.bSuccess)
		{
			TestNull(TEXT("and nothing started"), GEditor->PlayWorld.Get());
		}
		else
		{
			Play::RunPlayFromBody(TEXT("{\"action\": \"stop\"}"));
		}
		PluginSettings->bEnableExecutePython = State->bExecuteBefore;
		PluginSettings->bEnableDeleteAssets = State->bDeleteBefore;
		DiscardBlueprint(State->Blueprint.Get());
		State->Blueprint.Reset();
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}));
	return true;
}

// "set", "setnopec" and "get" reach only objects of the play session. Their target is read and looked up as the engine
// reads it (the object name unescaped, a class first), and its identity decides: a class default object (the plugin's
// settings named with an escaped character, another class's defaults), an editor-world actor and a config class
// (a class-wide set saves its config file) are refused, at start, in the session and through a key binding; an actor
// of the play world is not. "get" always reads class defaults and is refused for a class that exists.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayPropertyCommandTargetsTest,
	"UEAssetDoc.Level.Play.PropertyCommandTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayPropertyCommandTargetsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPlayToolsTests;
	UAssetDocumentSettings* PluginSettings = UAssetDocumentSettings::Get();
	UGameMapsSettings* MapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (!TestNotNull(TEXT("the plugin settings"), PluginSettings) || !TestNotNull(TEXT("the maps settings"), MapsSettings))
	{
		return false;
	}
	// The escaped name unescapes (FParse::Token) to Default__AssetDocumentSettings.
	const FString EscapedSettings = TEXT("set \"Default__AssetDocument\\Settings\" bEnableExecutePython true");
	TSharedRef<FProtectedSettingsState> State = MakeShared<FProtectedSettingsState>();
	State->bExecuteBefore = PluginSettings->bEnableExecutePython;
	PluginSettings->bEnableExecutePython = false;
	const bool bSplitscreenBefore = MapsSettings->bUseSplitscreen;

	const auto ConsoleBody = [](const FString& Command)
	{
		return FString::Printf(TEXT("{\"action\": \"console\", \"command\": \"%s\"}"), *Command.ReplaceCharWithEscapedChar());
	};
	const auto Refused = [](const FLevelToolResult& Result)
	{
		return !Result.bSuccess && (HasCode(Result, TEXT("Play.TargetOutsidePlay")) || HasCode(Result, TEXT("Play.CommandRefused")));
	};

	// Before a session: the escaped name is refused at start, and nothing starts.
	const FLevelToolResult StartRefused = Play::RunPlayFromBody(FString::Printf(TEXT("{\"action\": \"start\", \"console\": [\"%s\"]}"),
		*EscapedSettings.ReplaceCharWithEscapedChar()));
	TestTrue(*FString::Printf(TEXT("a start command with the escaped settings name is refused (%s)"), *Codes(StartRefused)), Refused(StartRefused));
	TestNull(TEXT("and nothing started"), GEditor->PlayWorld.Get());

	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	UWorld* World = GetEditorWorld();
	State->Blueprint.Reset(NewHolderBlueprint());
	AActor* Holder = World != nullptr && State->Blueprint->IsUpToDate()
		? SpawnActor(*World->PersistentLevel, State->Blueprint->GeneratedClass, FVector(0.0, 0.0, 0.0), TEXT("TargetHolder")) : nullptr;
	if (!TestNotNull(TEXT("the holder is placed"), Holder))
	{
		DiscardBlueprint(State->Blueprint.Get());
		PluginSettings->bEnableExecutePython = State->bExecuteBefore;
		return false;
	}
	const FString EditorHolderPath = Holder->GetPathName();

	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"start\", \"paused\": true}"),
		[this, PluginSettings, MapsSettings, bSplitscreenBefore, EscapedSettings, EditorHolderPath, ConsoleBody, Refused](const FLevelToolResult& Result)
	{
		if (!TestTrue(*FString::Printf(TEXT("start succeeds (%s)"), *Codes(Result)), Result.bSuccess))
		{
			return;
		}
		UWorld* PlayWorld = GEditor->PlayWorld;
		AActor* PlayHolder = FindPlayActor(TEXT("TargetHolder"));
		if (!TestNotNull(TEXT("the play world and its holder"), PlayHolder))
		{
			return;
		}

		// The exact escaped input, typed, and bound to a key.
		const FLevelToolResult Escaped = Play::RunPlayFromBody(ConsoleBody(EscapedSettings));
		TestTrue(*FString::Printf(TEXT("the escaped settings name is refused (%s)"), *Codes(Escaped)), Refused(Escaped));
		TestTrue(TEXT("the guard refuses it as a target outside play"),
			Play::RefusedConsoleCommand(EscapedSettings, PlayWorld).Code == TEXT("Play.TargetOutsidePlay"));
		APlayerController* PlayController = PlayWorld->GetFirstPlayerController();
		if (TestTrue(TEXT("the play controller has a player input"), PlayController != nullptr && PlayController->PlayerInput != nullptr))
		{
			FKeyBind Bind;
			Bind.Key = EKeys::F10;
			Bind.Command = EscapedSettings;
			PlayController->PlayerInput->DebugExecBindings.Add(Bind);
			const FLevelToolResult Pressed = Play::RunPlayFromBody(TEXT("{\"action\": \"input\", \"inputs\": [{\"key\": \"F10\"}]}"));
			PlayController->PlayerInput->DebugExecBindings.RemoveAll([](const FKeyBind& Entry) { return Entry.Key == EKeys::F10; });
			TestTrue(*FString::Printf(TEXT("a key bound to it is not pressed (%s)"), *Codes(Pressed)), Refused(Pressed));
		}
		TestFalse(TEXT("execute_python stays off"), PluginSettings->bEnableExecutePython);

		// Another class's default object, plain and escaped, and "get" of class defaults.
		for (const TCHAR* Command : {TEXT("set Default__GameMapsSettings bUseSplitscreen true"),
			TEXT("setnopec \"Default__GameMaps\\Settings\" bUseSplitscreen true"), TEXT("get GameMapsSettings bUseSplitscreen"),
			TEXT("slomo 1 | get \"GameMaps\\Settings\" bUseSplitscreen")})
		{
			const FLevelToolResult Other = Play::RunPlayFromBody(ConsoleBody(Command));
			TestTrue(*FString::Printf(TEXT("'%s' is refused as outside play (%s)"), Command, *Codes(Other)),
				!Other.bSuccess && HasCode(Other, TEXT("Play.TargetOutsidePlay")));
		}
		TestEqual(TEXT("the maps settings are unchanged"), MapsSettings->bUseSplitscreen, bSplitscreenBefore);

		// The same actor in the editor world, and a class-wide set of a config class (it saves the config file).
		for (const FString& Command : {FString::Printf(TEXT("set %s bTouched true"), *EditorHolderPath),
			FString(TEXT("set PlayerController bShowMouseCursor true"))})
		{
			const FLevelToolResult Outside = Play::RunPlayFromBody(ConsoleBody(Command));
			TestTrue(*FString::Printf(TEXT("'%s' is refused as outside play (%s)"), *Command, *Codes(Outside)),
				!Outside.bSuccess && HasCode(Outside, TEXT("Play.TargetOutsidePlay")));
		}

		// The play world's actor, and a class-wide set of a class without config: not refused.
		const FString PlaySet = FString::Printf(TEXT("set %s bTouched true"), *PlayHolder->GetPathName());
		TestFalse(TEXT("the guard lets a set of a play-world actor through"), Play::RefusedConsoleCommand(PlaySet, PlayWorld).IsRefused());
		const FLevelToolResult InPlay = Play::RunPlayFromBody(ConsoleBody(PlaySet));
		TestTrue(*FString::Printf(TEXT("a set of a play-world actor runs (%s)"), *Codes(InPlay)), InPlay.bSuccess);
		if (InPlay.Payload.IsValid())
		{
			AddInfo(FString::Printf(TEXT("the console printed: %s"), *InPlay.Payload->GetStringField(TEXT("output"))));
		}
		TestTrue(TEXT("the play holder is a play session object"), Play::IsPlaySessionObject(*PlayHolder, PlayWorld));
		TestTrue(TEXT("the play game instance is a play session object"),
			PlayWorld->GetGameInstance() != nullptr && Play::IsPlaySessionObject(*PlayWorld->GetGameInstance(), PlayWorld));
		const UClass* PlainClass = UKismetMathLibrary::StaticClass();
		if (TestFalse(TEXT("the math library class has no config"), PlainClass->HasAnyClassFlags(CLASS_Config)))
		{
			TestFalse(TEXT("a class-wide set of a class without config is not refused"),
				Play::RefusedConsoleCommand(TEXT("set KismetMathLibrary NoSuchValue 1"), PlayWorld).IsRefused());
		}
		TestFalse(TEXT("a set of a name that resolves to nothing is not refused"),
			Play::RefusedConsoleCommand(TEXT("set NoSuchObjectAnywhere Value 1"), PlayWorld).IsRefused());
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FPlayRequestCommand(TEXT("{\"action\": \"stop\"}"), [this, State, PluginSettings](const FLevelToolResult& Result)
	{
		TestTrue(*FString::Printf(TEXT("stop (%s)"), *Codes(Result)), Result.bSuccess);
		TestFalse(TEXT("execute_python stays off after play"), PluginSettings->bEnableExecutePython);
		PluginSettings->bEnableExecutePython = State->bExecuteBefore;
		DiscardBlueprint(State->Blueprint.Get());
		State->Blueprint.Reset();
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}));
	return true;
}


// The player's console runs every command of a line, read as UE reads it: split at '|' and line breaks, a command
// name ending at any character that is not a letter or digit, after any whitespace. Python and the commands that hand
// text on as another command are refused in every command and every word of the line (also with execute_python
// switched on), "ke"/"ce" do not call functions that run commands or Python, and ordinary game commands pass. All
// checked before a session is needed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPlayCommandGuardTest,
	"UEAssetDoc.Level.Play.CommandGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPlayCommandGuardTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPlayToolsTests;
	UAssetDocumentSettings* PluginSettings = UAssetDocumentSettings::Get();
	if (!TestNotNull(TEXT("the plugin settings"), PluginSettings))
	{
		return false;
	}
	// The refusal does not depend on the Python tool's switch.
	TGuardValue<bool> PythonOn(PluginSettings->bEnableExecutePython, true);

	TArray<FString> Refused = {
		TEXT("py\timport unreal"),                    // a tab after py
		TEXT("py\vimport unreal"),                    // other whitespace
		TEXT("py\fimport unreal"),
		TEXT("py\"import unreal\""),                  // a quote ends the name too
		TEXT("PY.exec import unreal"),
		TEXT("slomo 1\npy import unreal"),            // a line break separates commands
		TEXT("slomo 1\r\npy import unreal"),
		TEXT("slomo 1|py\timport unreal"),            // '|' separates commands
		TEXT("slomo 1 // comment | py\timport unreal"),
		TEXT("ke * ServerExec slomo 1"),              // ke / ce call a function that runs a command
		TEXT("ke\t*\tServerExec\tslomo 1"),
		TEXT("KismetEvent * SendToConsole slomo 1"),
		TEXT("ce ServerExec slomo 1"),
		TEXT("ke * \"ExecuteConsoleCommand\" None slomo"),
		TEXT("ServerExec slomo 1"),                   // functions and commands that hand text on
		TEXT("\tServerExec\tslomo 1"),
		TEXT("\"ServerExec\" slomo 1"),
		TEXT("defer py import unreal"),
		TEXT("SetBind F9 py\timport unreal"),
		TEXT("SetBind F9 \"p\\y import unreal\""),    // an escaped word that unescapes to py
		TEXT("SetBind F9 \"\" \"p\\y import unreal\""),
		TEXT("exec commands.txt"),
		TEXT("quit"),
		TEXT("slomo 1\nexit")};
	if (FindObject<UClass>(nullptr, TEXT("/Script/PythonScriptPlugin.PythonScriptLibrary")) != nullptr)
	{
		// A Python script library function, found by its name.
		Refused.Add(TEXT("ke Default__PythonScriptLibrary ExecutePythonCommand \"import unreal\""));
	}
	const auto RunConsole = [](const FString& Command)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("action"), TEXT("console"));
		Body->SetStringField(TEXT("command"), Command);
		FString Text;
		FJsonSerializer::Serialize(Body, TJsonWriterFactory<>::Create(&Text));
		return Play::RunPlayFromBody(Text);
	};
	for (const FString& Command : Refused)
	{
		const FLevelToolResult Console = RunConsole(Command);
		TestTrue(*FString::Printf(TEXT("console '%s' is refused (%s)"), *Command.ReplaceCharWithEscapedChar(), *Codes(Console)),
			HasCode(Console, TEXT("Play.CommandRefused")));
	}
	const FLevelToolResult Start = Play::RunPlayFromBody(TEXT("{\"action\": \"start\", \"console\": [\"slomo 1\", \"py\\timport unreal\"]}"));
	TestTrue(*FString::Printf(TEXT("a start console command is refused (%s)"), *Codes(Start)), HasCode(Start, TEXT("Play.CommandRefused")));
	TestNull(TEXT("and nothing started"), GEditor->PlayWorld.Get());

	for (const TCHAR* Command : {TEXT("slomo 0.5"), TEXT("stat fps"), TEXT("god | slomo 2"), TEXT("showdebug ai"), TEXT("summon BP_Enemy_C"),
		TEXT("ke Boss SetAutoAttack false"), TEXT("ke * SetActorHiddenInGame true"), TEXT("r.ScreenPercentage 50"), TEXT("ce OpenGate"),
		TEXT("pyramid 1"), TEXT("ke Door Open \"copy\"")})
	{
		// Checked, then refused only for want of a session.
		const FLevelToolResult Console = RunConsole(Command);
		TestTrue(*FString::Printf(TEXT("'%s' passes the check (%s)"), Command, *Codes(Console)),
			!HasCode(Console, TEXT("Play.CommandRefused")) && HasCode(Console, TEXT("Play.NoSession")));
	}
	return true;
}

#endif
