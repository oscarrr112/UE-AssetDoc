// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Play/PlayInput.h"

#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputDeveloperSettings.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "Play/PlayQuery.h"
#include "UserSettings/EnhancedInputUserSettings.h"

namespace UE::AssetDocumentLevel::Private::Play
{
namespace PlayInputLocal
{
// UEnhancedPlayerInput keeps its live mappings and applied contexts protected; both are reflected properties, read
// here as the Details panel / Gameplay Debugger would (read-only).
const TArray<FEnhancedActionKeyMapping>& LiveMappings(const UEnhancedPlayerInput& PlayerInput)
{
	static const TArray<FEnhancedActionKeyMapping> None;
	const FArrayProperty* Property = FindFProperty<FArrayProperty>(UEnhancedPlayerInput::StaticClass(), TEXT("EnhancedActionMappings"));
	const FStructProperty* Inner = Property != nullptr ? CastField<FStructProperty>(Property->Inner) : nullptr;
	return Inner != nullptr && Inner->Struct == FEnhancedActionKeyMapping::StaticStruct()
		? *Property->ContainerPtrToValuePtr<TArray<FEnhancedActionKeyMapping>>(&PlayerInput) : None;
}

const TMap<TObjectPtr<const UInputMappingContext>, FAppliedInputContextData>& AppliedContexts(const UEnhancedPlayerInput& PlayerInput)
{
	static const TMap<TObjectPtr<const UInputMappingContext>, FAppliedInputContextData> None;
	const FMapProperty* Property = FindFProperty<FMapProperty>(UEnhancedPlayerInput::StaticClass(), TEXT("AppliedInputContextData"));
	const FStructProperty* Value = Property != nullptr ? CastField<FStructProperty>(Property->ValueProp) : nullptr;
	return Value != nullptr && Value->Struct == FAppliedInputContextData::StaticStruct()
		? *Property->ContainerPtrToValuePtr<TMap<TObjectPtr<const UInputMappingContext>, FAppliedInputContextData>>(&PlayerInput) : None;
}

UEnhancedInputLocalPlayerSubsystem* PlayerInputSubsystem(UWorld& World, APlayerController*& OutController)
{
	OutController = GetLocalPlayerController(World);
	return OutController != nullptr ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(OutController->GetLocalPlayer()) : nullptr;
}

// The key profile whose keys the mapping rebuild uses, when user settings are on.
const UEnhancedPlayerMappableKeyProfile* ActiveKeyProfile(const UEnhancedInputLocalPlayerSubsystem& Subsystem)
{
	if (!GetDefault<UEnhancedInputDeveloperSettings>()->bEnableUserSettings)
	{
		return nullptr;
	}
	const UEnhancedInputUserSettings* Settings = Subsystem.GetUserSettings();
	return Settings != nullptr ? Settings->GetActiveKeyProfile() : nullptr;
}

// The keys a context's mapping is live under, as IEnhancedInputSubsystemInterface::RebuildControlMappings picks
// them: the player's mapped keys for a player-mappable mapping, else the asset's key.
TArray<FKey> EffectiveKeys(const FEnhancedActionKeyMapping& Mapping, const UEnhancedPlayerMappableKeyProfile* Profile)
{
	TArray<FKey> Keys;
	if (Profile != nullptr)
	{
		Profile->GetPlayerMappedKeysForRebuildControlMappings(Mapping, Keys);
	}
	if (Keys.IsEmpty())
	{
		Keys.Add(Mapping.Key);
	}
	return Keys;
}

const TArray<FEnhancedActionKeyMapping>& ContextMappings(const UInputMappingContext& Context, const UEnhancedPlayerMappableKeyProfile* Profile)
{
	return Context.GetMappingsForProfile(Profile != nullptr ? Profile->GetProfileIdString() : FString());
}

// The player's applied contexts, highest priority first (ties by name, for a stable answer).
TArray<TPair<const UInputMappingContext*, int32>> OrderedContexts(const UEnhancedPlayerInput& PlayerInput)
{
	TArray<TPair<const UInputMappingContext*, int32>> Contexts;
	for (const TPair<TObjectPtr<const UInputMappingContext>, FAppliedInputContextData>& Pair : PlayInputLocal::AppliedContexts(PlayerInput))
	{
		if (Pair.Key != nullptr)
		{
			Contexts.Emplace(Pair.Key.Get(), Pair.Value.Priority);
		}
	}
	Contexts.Sort([](const TPair<const UInputMappingContext*, int32>& A, const TPair<const UInputMappingContext*, int32>& B)
	{
		return A.Value != B.Value ? A.Value > B.Value : A.Key->GetName() < B.Key->GetName();
	});
	return Contexts;
}

bool IsLive(const UEnhancedPlayerInput& PlayerInput, const UInputAction* Action, const FKey& Key)
{
	return PlayInputLocal::LiveMappings(PlayerInput).ContainsByPredicate([Action, &Key](const FEnhancedActionKeyMapping& Live)
	{
		return Live.Action == Action && Live.Key == Key;
	});
}

FString SlotName(EPlayerMappableKeySlot Slot)
{
	const UEnum* Enum = StaticEnum<EPlayerMappableKeySlot>();
	return Enum != nullptr ? Enum->GetNameStringByValue(static_cast<int64>(Slot)) : FString::FromInt(static_cast<int32>(Slot));
}

// Every customized player mapping of every key profile of Settings.
TArray<const FPlayerKeyMapping*> CustomizedMappings(const UEnhancedInputUserSettings& Settings)
{
	TArray<const FPlayerKeyMapping*> Out;
	for (const TPair<FString, TObjectPtr<UEnhancedPlayerMappableKeyProfile>>& Profile : Settings.GetAllAvailableKeyProfiles())
	{
		if (Profile.Value == nullptr)
		{
			continue;
		}
		for (const TPair<FName, FKeyMappingRow>& Row : Profile.Value->GetPlayerMappingRows())
		{
			for (const FPlayerKeyMapping& Mapping : Row.Value.Mappings)
			{
				if (Mapping.IsCustomized())
				{
					Out.Add(&Mapping);
				}
			}
		}
	}
	return Out;
}

FString SaveFileText(const FString& Slot)
{
	return FString::Printf(TEXT("Saved/SaveGames/%s.sav"), *Slot);
}

void SaveSlot(UEnhancedInputLocalPlayerSubsystem* Subsystem, APlayerController* Controller, FString& OutSlot, int32& OutUserIndex)
{
	OutSlot = GetDefault<UEnhancedInputDeveloperSettings>()->InputSettingsSaveSlotName;
	OutUserIndex = 0;
	if (Subsystem != nullptr)
	{
		if (const UEnhancedPlayerInput* PlayerInput = Subsystem->GetPlayerInput())
		{
			OutSlot = PlayerInput->GetUserSettingsSaveFileName();
		}
	}
	if (Controller != nullptr && Controller->GetLocalPlayer() != nullptr)
	{
		OutUserIndex = Controller->GetLocalPlayer()->GetLocalPlayerIndex();
	}
}
}

bool InjectedKeyReachesMapping(const FKey& Key, const FKey& MappingKey)
{
	if (!Key.IsValid() || !MappingKey.IsValid())
	{
		return false;
	}
	// MouseX reaches Mouse2D (its paired axis key); Mouse2D (sent as its X and Y keys) reaches MouseX / MouseY.
	return Key == MappingKey || (Key.GetPairedAxisKey().IsValid() && Key.GetPairedAxisKey() == MappingKey)
		|| (MappingKey.GetPairedAxisKey().IsValid() && MappingKey.GetPairedAxisKey() == Key);
}

TArray<FLiveKeyReach> FindLiveMappingsForKey(const UEnhancedPlayerInput& PlayerInput, const FKey& Key)
{
	TArray<FLiveKeyReach> Out;
	for (const FEnhancedActionKeyMapping& Mapping : PlayInputLocal::LiveMappings(PlayerInput))
	{
		if (Mapping.Action == nullptr || !InjectedKeyReachesMapping(Key, Mapping.Key))
		{
			continue;
		}
		if (!Out.ContainsByPredicate([&Mapping](const FLiveKeyReach& Reach) { return Reach.Action.Get() == Mapping.Action.Get(); }))
		{
			FLiveKeyReach& Reach = Out.AddDefaulted_GetRef();
			Reach.Action = Mapping.Action.Get();
			Reach.MappingKey = Mapping.Key;
		}
	}
	return Out;
}

TArray<FLiveKeyReach> FindPendingMappingsForKey(UEnhancedInputLocalPlayerSubsystem& Subsystem, const FKey& Key)
{
	using namespace PlayInputLocal;
	TArray<FLiveKeyReach> Out;
	const UEnhancedPlayerInput* PlayerInput = Subsystem.GetPlayerInput();
	if (PlayerInput == nullptr)
	{
		return Out;
	}
	const UEnhancedPlayerMappableKeyProfile* Profile = ActiveKeyProfile(Subsystem);
	const TArray<TPair<const UInputMappingContext*, int32>> Contexts = OrderedContexts(*PlayerInput);
	for (int32 Index = 0; Index < Contexts.Num(); ++Index)
	{
		for (const FEnhancedActionKeyMapping& Mapping : ContextMappings(*Contexts[Index].Key, Profile))
		{
			if (Mapping.Action == nullptr || !InjectedKeyReachesMapping(Key, Mapping.Key) || !EffectiveKeys(Mapping, Profile).Contains(Mapping.Key))
			{
				continue;
			}
			// A higher-priority context mapping the same key explains a missing live mapping.
			bool bTaken = false;
			for (int32 Higher = 0; Higher < Index && !bTaken; ++Higher)
			{
				if (Contexts[Higher].Value <= Contexts[Index].Value)
				{
					continue;
				}
				for (const FEnhancedActionKeyMapping& Other : ContextMappings(*Contexts[Higher].Key, Profile))
				{
					if (Other.Action != nullptr && EffectiveKeys(Other, Profile).Contains(Mapping.Key))
					{
						bTaken = true;
						break;
					}
				}
			}
			if (!bTaken && !Out.ContainsByPredicate([&Mapping](const FLiveKeyReach& Reach) { return Reach.Action.Get() == Mapping.Action.Get(); }))
			{
				FLiveKeyReach& Reach = Out.AddDefaulted_GetRef();
				Reach.Action = Mapping.Action.Get();
				Reach.MappingKey = Mapping.Key;
			}
		}
	}
	return Out;
}

FString DescribeUnreachedKey(UEnhancedInputLocalPlayerSubsystem& Subsystem, const FKey& Key)
{
	using namespace PlayInputLocal;
	const UEnhancedPlayerInput* PlayerInput = Subsystem.GetPlayerInput();
	if (PlayerInput == nullptr)
	{
		return FString();
	}
	const UEnhancedPlayerMappableKeyProfile* Profile = ActiveKeyProfile(Subsystem);
	TArray<FString> Reasons;
	for (const TPair<const UInputMappingContext*, int32>& Context : OrderedContexts(*PlayerInput))
	{
		for (const FEnhancedActionKeyMapping& Mapping : ContextMappings(*Context.Key, Profile))
		{
			if (Mapping.Action == nullptr || !InjectedKeyReachesMapping(Key, Mapping.Key))
			{
				continue;
			}
			const TArray<FKey> Keys = EffectiveKeys(Mapping, Profile);
			FString Why;
			if (!Keys.Contains(Mapping.Key))
			{
				TArray<FString> Names;
				for (const FKey& Effective : Keys)
				{
					Names.Add(Effective.GetFName().ToString());
				}
				Why = FString::Printf(TEXT("the player's key settings map it to %s"), *FString::Join(Names, TEXT(", ")));
			}
			else
			{
				Why = TEXT("a higher-priority context takes the key");
			}
			Reasons.AddUnique(FString::Printf(TEXT("%s maps it to %s, but %s"), *Context.Key->GetName(), *Mapping.Action->GetName(), *Why));
		}
	}
	return FString::Join(Reasons, TEXT("; "));
}

TArray<FString> DescribeCustomizedPlayerKeys(UEnhancedInputLocalPlayerSubsystem& Subsystem)
{
	TArray<FString> Out;
	if (const UEnhancedInputUserSettings* Settings = Subsystem.GetUserSettings())
	{
		for (const FPlayerKeyMapping* Mapping : PlayInputLocal::CustomizedMappings(*Settings))
		{
			Out.Add(FString::Printf(TEXT("%s: %s -> %s"), *Mapping->GetMappingName().ToString(), *Mapping->GetDefaultKey().GetFName().ToString(),
				*Mapping->GetCurrentKey().GetFName().ToString()));
		}
	}
	Out.Sort();
	return Out;
}

FLevelToolResult QueryPlayInput(UWorld& World)
{
	using namespace PlayInputLocal;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	APlayerController* Controller = nullptr;
	UEnhancedInputLocalPlayerSubsystem* Subsystem = PlayerInputSubsystem(World, Controller);
	const UEnhancedPlayerInput* PlayerInput = Subsystem != nullptr ? Subsystem->GetPlayerInput() : nullptr;
	if (PlayerInput == nullptr)
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.NoPlayer"), Controller == nullptr
			? TEXT("The play session has no local player (Simulate has none): input needs \"mode\": \"window\" or \"viewport\".")
			: TEXT("The local player has no Enhanced Input player input.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	const UEnhancedPlayerMappableKeyProfile* Profile = ActiveKeyProfile(*Subsystem);
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("controller"), Controller->GetName());

	// Keys an earlier (higher-priority) context consumes, as the mapping rebuild applies them.
	TMap<FKey, FString> TakenBy;
	TArray<TSharedPtr<FJsonValue>> ContextValues;
	for (const TPair<const UInputMappingContext*, int32>& Context : OrderedContexts(*PlayerInput))
	{
		TSharedRef<FJsonObject> ContextJson = MakeShared<FJsonObject>();
		ContextJson->SetStringField(TEXT("context"), Context.Key->GetPathName());
		ContextJson->SetNumberField(TEXT("priority"), Context.Value);
		TArray<TSharedPtr<FJsonValue>> MappingValues;
		TMap<FKey, FString> ContextTaken;
		for (const FEnhancedActionKeyMapping& Mapping : ContextMappings(*Context.Key, Profile))
		{
			if (Mapping.Action == nullptr)
			{
				continue;
			}
			for (const FKey& Key : EffectiveKeys(Mapping, Profile))
			{
				TSharedRef<FJsonObject> MappingJson = MakeShared<FJsonObject>();
				MappingJson->SetStringField(TEXT("action"), Mapping.Action->GetName());
				MappingJson->SetStringField(TEXT("key"), Key.GetFName().ToString());
				if (Key != Mapping.Key)
				{
					// The player's key settings moved it from the asset's key.
					MappingJson->SetStringField(TEXT("asset_key"), Mapping.Key.GetFName().ToString());
				}
				const bool bLive = IsLive(*PlayerInput, Mapping.Action, Key);
				MappingJson->SetBoolField(TEXT("live"), bLive);
				if (!bLive)
				{
					if (const FString* Taker = TakenBy.Find(Key))
					{
						MappingJson->SetStringField(TEXT("taken_by"), *Taker);
					}
				}
				else if (Mapping.Action->bConsumeInput)
				{
					ContextTaken.Add(Key, Context.Key->GetName());
				}
				MappingValues.Add(MakeShared<FJsonValueObject>(MappingJson));
			}
		}
		TakenBy.Append(ContextTaken);
		ContextJson->SetArrayField(TEXT("mappings"), MappingValues);
		ContextValues.Add(MakeShared<FJsonValueObject>(ContextJson));
	}
	Payload->SetArrayField(TEXT("contexts"), ContextValues);
	Payload->SetNumberField(TEXT("live_mappings"), PlayInputLocal::LiveMappings(*PlayerInput).Num());

	TSharedRef<FJsonObject> UserJson = MakeShared<FJsonObject>();
	const bool bEnabled = GetDefault<UEnhancedInputDeveloperSettings>()->bEnableUserSettings;
	UserJson->SetBoolField(TEXT("enabled"), bEnabled);
	FString Slot;
	int32 UserIndex = 0;
	SaveSlot(Subsystem, Controller, Slot, UserIndex);
	const bool bSaved = UGameplayStatics::DoesSaveGameExist(Slot, UserIndex);
	UserJson->SetStringField(TEXT("save_file"), SaveFileText(Slot));
	UserJson->SetBoolField(TEXT("saved"), bSaved);
	TArray<TSharedPtr<FJsonValue>> Customized;
	if (const UEnhancedInputUserSettings* Settings = Subsystem->GetUserSettings())
	{
		if (const UEnhancedPlayerMappableKeyProfile* Active = Settings->GetActiveKeyProfile())
		{
			UserJson->SetStringField(TEXT("profile"), Active->GetProfileIdString());
		}
		for (const FPlayerKeyMapping* Mapping : CustomizedMappings(*Settings))
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("mapping"), Mapping->GetMappingName().ToString());
			Entry->SetStringField(TEXT("slot"), SlotName(Mapping->GetSlot()));
			if (const UInputAction* Action = Mapping->GetAssociatedInputAction())
			{
				Entry->SetStringField(TEXT("action"), Action->GetName());
			}
			Entry->SetStringField(TEXT("default_key"), Mapping->GetDefaultKey().GetFName().ToString());
			Entry->SetStringField(TEXT("key"), Mapping->GetCurrentKey().GetFName().ToString());
			Customized.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	UserJson->SetArrayField(TEXT("customized"), Customized);
	Payload->SetObjectField(TEXT("user_settings"), UserJson);

	FLevelToolResult Result = FLevelToolResult::Success(FString::Printf(TEXT("%d active mapping context(s), %d live mapping(s)."),
		ContextValues.Num(), PlayInputLocal::LiveMappings(*PlayerInput).Num()), Payload);
	if (!Customized.IsEmpty() && bEnabled)
	{
		FAssetDocumentDiagnostic Warning = MakePlayDiagnostic(TEXT("/user_settings"), TEXT("Play.InputUserSettingsOverride"), FString::Printf(
			TEXT("The player's key settings remap %d mapping(s) away from the assets' keys (user_settings.customized)%s; every session loads them. "
				"play {\"action\": \"reset_input_settings\"} resets them to the assets' keys."), Customized.Num(),
			bSaved ? *FString::Printf(TEXT(", saved in %s"), *SaveFileText(Slot)) : TEXT("")));
		Warning.Severity = TEXT("warning");
		Result.Diagnostics.Add(MoveTemp(Warning));
	}
	return Result;
}

FLevelToolResult ResetPlayInputSettings(UWorld* World)
{
	using namespace PlayInputLocal;
	APlayerController* Controller = nullptr;
	UEnhancedInputLocalPlayerSubsystem* Subsystem = World != nullptr ? PlayerInputSubsystem(*World, Controller) : nullptr;
	FString Slot;
	int32 UserIndex = 0;
	SaveSlot(Subsystem, Controller, Slot, UserIndex);
	TArray<FString> Reset;
	if (Subsystem != nullptr)
	{
		if (UEnhancedInputUserSettings* Settings = Subsystem->GetUserSettings())
		{
			Reset = DescribeCustomizedPlayerKeys(*Subsystem);
			for (const TPair<FString, TObjectPtr<UEnhancedPlayerMappableKeyProfile>>& Profile : Settings->GetAllAvailableKeyProfiles())
			{
				if (Profile.Value != nullptr)
				{
					Profile.Value->ResetToDefault();
				}
			}
			Settings->ApplySettings();
			Subsystem->RequestRebuildControlMappings();
		}
	}
	const bool bExisted = UGameplayStatics::DoesSaveGameExist(Slot, UserIndex);
	const bool bDeleted = bExisted && UGameplayStatics::DeleteGameInSlot(Slot, UserIndex);
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("save_file"), SaveFileText(Slot));
	Payload->SetBoolField(TEXT("deleted"), bDeleted);
	Payload->SetArrayField(TEXT("reset"), PlayStringArray(Reset));
	if (bExisted && !bDeleted)
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.InputSettingsNotDeleted"),
			FString::Printf(TEXT("%s exists but could not be deleted."), *SaveFileText(Slot))));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics), Payload);
	}
	return FLevelToolResult::Success(FString::Printf(TEXT("%s%s."), Subsystem != nullptr
		? *FString::Printf(TEXT("Reset %d player key mapping(s) to the assets' keys"), Reset.Num()) : TEXT("No session: only the saved settings"),
		bDeleted ? *FString::Printf(TEXT("; deleted %s"), *SaveFileText(Slot)) : TEXT("; no saved settings file")), Payload);
}
}
