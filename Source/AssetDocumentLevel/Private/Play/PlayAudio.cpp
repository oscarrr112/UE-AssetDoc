// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Play/PlayAudio.h"

#include "ActiveSound.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Play/PlayLog.h"
#include "Play/PlayQuery.h"
#include "Play/PlaySession.h"
#include "Sound/SoundBase.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocumentLevel::Private::Play::PlayAudioLocal
{
constexpr int32 AudioRecordCapacity = 2000;
constexpr int32 AudioDefaultLimit = 50;
constexpr int32 AudioMaximumLimit = 500;

// One sound the audio engine started. Times are FPlatformTime::Seconds (real time, as the log's).
struct FSoundStart
{
	uint64 Seq = 0;
	uint32 PlayOrder = 0;
	uint32 WorldId = 0;
	double Started = 0.0;
	double Finished = -1.0;
	FString Sound;
	FString Owner;
	FString Component;
	FVector Location = FVector::ZeroVector;
	bool bVirtual = false;
	bool bUi = false;
};

class FSoundStartRecord
{
public:
	static FSoundStartRecord& Get()
	{
		static FSoundStartRecord Instance;
		return Instance;
	}

	// A sound became audible (bVirtual false) or started virtual (inaudible: out of range, or over its concurrency
	// limit). A sound that changes between the two keeps its entry (same play order) and its number.
	void Started(const FActiveSound& ActiveSound, bool bVirtual)
	{
		if (ActiveSound.IsPreviewSound())
		{
			return;
		}
		FScopeLock ScopeLock(&Lock);
		if (FSoundStart* Existing = Find(ActiveSound.GetPlayOrder(), ActiveSound.GetWorldID()))
		{
			Existing->bVirtual = bVirtual;
			Existing->Finished = -1.0;
			return;
		}
		FSoundStart Entry;
		Entry.Seq = FPlayLog::Get().ReserveSeq();
		Entry.PlayOrder = ActiveSound.GetPlayOrder();
		Entry.WorldId = ActiveSound.GetWorldID();
		Entry.Started = FPlatformTime::Seconds();
		const USoundBase* Sound = ActiveSound.GetSound();
		Entry.Sound = Sound != nullptr ? Sound->GetPathName() : FString();
		Entry.Owner = ActiveSound.GetOwnerID() > 0 ? ActiveSound.GetOwnerName() : FString();
		Entry.Component = ActiveSound.GetAudioComponentID() > 0 ? ActiveSound.GetAudioComponentName() : FString();
		Entry.Location = ActiveSound.Transform.GetLocation();
		Entry.bVirtual = bVirtual;
		Entry.bUi = ActiveSound.bIsUISound;
		if (Ring.Num() < AudioRecordCapacity)
		{
			Ring.Add(MoveTemp(Entry));
		}
		else
		{
			Ring[Head] = MoveTemp(Entry);
			Head = (Head + 1) % AudioRecordCapacity;
		}
	}

	// The audible (bVirtual false) or the virtual instance of a sound ended. The other instance's end belongs to a
	// change between audible and virtual, not to the sound's end.
	void Ended(const FActiveSound& ActiveSound, bool bVirtual)
	{
		FScopeLock ScopeLock(&Lock);
		FSoundStart* Existing = Find(ActiveSound.GetPlayOrder(), ActiveSound.GetWorldID());
		if (Existing != nullptr && Existing->bVirtual == bVirtual && Existing->Finished < 0.0)
		{
			Existing->Finished = FPlatformTime::Seconds();
		}
	}

	// The sounds of a world after Since, oldest first.
	TArray<FSoundStart> Query(uint32 WorldId, uint64 Since) const
	{
		FScopeLock ScopeLock(&Lock);
		TArray<FSoundStart> Out;
		const int32 Count = Ring.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FSoundStart& Entry = Ring[Count < AudioRecordCapacity ? Index : (Head + Index) % AudioRecordCapacity];
			if (Entry.WorldId == WorldId && Entry.Seq > Since)
			{
				Out.Add(Entry);
			}
		}
		return Out;
	}

private:
	FSoundStart* Find(uint32 PlayOrder, uint32 WorldId)
	{
		// Newest first: a sound changes between audible and virtual while it plays.
		for (int32 Offset = 0; Offset < Ring.Num(); ++Offset)
		{
			const int32 Index = (Head - 1 - Offset + Ring.Num()) % Ring.Num();
			if (Ring[Index].PlayOrder == PlayOrder && Ring[Index].WorldId == WorldId)
			{
				return &Ring[Index];
			}
		}
		return nullptr;
	}

	mutable FCriticalSection Lock;
	TArray<FSoundStart> Ring;
	int32 Head = 0;
};

double RoundAudioTime(double Value)
{
	return FMath::RoundToDouble(Value * 1000.0) / 1000.0;
}

FString AudioComponentText(const UAudioComponent& Component)
{
	const AActor* Owner = Component.GetOwner();
	return Owner != nullptr ? Owner->GetName() + TEXT(".") + Component.GetName() : Component.GetPathName();
}

bool MatchesAudioName(const FString& Pattern, const FString& Sound, const FString& Owner, const FString& Component)
{
	if (Pattern.IsEmpty())
	{
		return true;
	}
	// A sound by its path or its asset name (the object name after the last dot).
	const FString ShortSound = Sound.Mid(Sound.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
	return Sound.MatchesWildcard(Pattern) || ShortSound.MatchesWildcard(Pattern) || (!Owner.IsEmpty() && Owner.MatchesWildcard(Pattern))
		|| (!Component.IsEmpty() && Component.MatchesWildcard(Pattern));
}

bool ParseAudioNear(UWorld& World, const FJsonObject& Json, TOptional<FVector>& OutCenter, double& OutRadius,
	TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TSharedPtr<FJsonObject>* Near = nullptr;
	if (!Json.HasField(TEXT("near")))
	{
		return true;
	}
	if (!Json.TryGetObjectField(TEXT("near"), Near) || Near == nullptr || !Near->IsValid()
		|| !(*Near)->TryGetNumberField(TEXT("radius"), OutRadius) || OutRadius < 0.0)
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/near"), TEXT("'near' is {\"location\": {X,Y,Z} | \"actor\": <ref>, \"radius\": cm}.")));
		return false;
	}
	const TSharedPtr<FJsonObject>* Location = nullptr;
	if ((*Near)->TryGetObjectField(TEXT("location"), Location) && Location != nullptr && Location->IsValid())
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		(*Location)->TryGetNumberField(TEXT("X"), X);
		(*Location)->TryGetNumberField(TEXT("Y"), Y);
		(*Location)->TryGetNumberField(TEXT("Z"), Z);
		OutCenter = FVector(X, Y, Z);
		return true;
	}
	const FPlayActorResolution Center = ResolvePlayActor(World, (*Near)->TryGetField(TEXT("actor")));
	if (Center.Actor == nullptr)
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/near/actor"), Center.Code.IsEmpty() ? FString(TEXT("Play.InvalidRequest")) : Center.Code,
			Center.Message.IsEmpty() ? FString(TEXT("'near' needs \"location\" or \"actor\".")) : Center.Message));
		return false;
	}
	OutCenter = Center.Actor->GetActorLocation();
	return true;
}
} // namespace UE::AssetDocumentLevel::Private::Play::PlayAudioLocal

void UAssetDocumentPlayAudioRecorder::NotifyActiveSoundCreated(FActiveSound& ActiveSound)
{
	UE::AssetDocumentLevel::Private::Play::PlayAudioLocal::FSoundStartRecord::Get().Started(ActiveSound, false);
}

void UAssetDocumentPlayAudioRecorder::NotifyActiveSoundDeleting(const FActiveSound& ActiveSound)
{
	UE::AssetDocumentLevel::Private::Play::PlayAudioLocal::FSoundStartRecord::Get().Ended(ActiveSound, false);
}

void UAssetDocumentPlayAudioRecorder::NotifyVirtualizedSoundCreated(FActiveSound& ActiveSound)
{
	UE::AssetDocumentLevel::Private::Play::PlayAudioLocal::FSoundStartRecord::Get().Started(ActiveSound, true);
}

void UAssetDocumentPlayAudioRecorder::NotifyVirtualizedSoundDeleting(const FActiveSound& ActiveSound)
{
	UE::AssetDocumentLevel::Private::Play::PlayAudioLocal::FSoundStartRecord::Get().Ended(ActiveSound, true);
}

namespace UE::AssetDocumentLevel::Private::Play
{
FLevelToolResult QueryAudio(UWorld& World, const FJsonObject& Json)
{
	using namespace PlayAudioLocal;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	uint64 SessionSeq = 0;
	double SessionTime = 0.0;
	GetSessionLogBase(SessionSeq, SessionTime);
	uint64 Since = SessionSeq;
	double SinceNumber = 0.0;
	FString SinceText;
	if (Json.TryGetNumberField(TEXT("since"), SinceNumber))
	{
		Since = SinceNumber < 0.0 ? 0 : static_cast<uint64>(SinceNumber);
	}
	else if (Json.TryGetStringField(TEXT("since"), SinceText))
	{
		if (SinceText == TEXT("all"))
		{
			Since = 0;
		}
		else if (SinceText != TEXT("session"))
		{
			Diagnostics.Add(PlayInvalidRequest(TEXT("/since"), TEXT("'since' is a seq number (an earlier result's last_seq), \"session\" (default) or \"all\".")));
		}
	}
	FString Name;
	Json.TryGetStringField(TEXT("name"), Name);
	TOptional<FVector> Center;
	double Radius = 0.0;
	ParseAudioNear(World, Json, Center, Radius, Diagnostics);
	int32 Limit = AudioDefaultLimit;
	double LimitNumber = 0.0;
	if (Json.TryGetNumberField(TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, AudioMaximumLimit);
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	const auto IsNear = [&Center, Radius](const FVector& Location)
	{
		return !Center.IsSet() || FVector::Dist(Location, Center.GetValue()) <= Radius;
	};

	// Audio components that are playing now (a component's sound also starts an entry below).
	TArray<TSharedPtr<FJsonValue>> Components;
	for (TObjectIterator<UAudioComponent> It; It; ++It)
	{
		UAudioComponent* Component = *It;
		if (!IsValid(Component) || Component->GetWorld() != &World || Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
			|| !Component->IsPlaying())
		{
			continue;
		}
		const AActor* Owner = Component->GetOwner();
		const FString SoundPath = Component->Sound != nullptr ? Component->Sound->GetPathName() : FString();
		if (!MatchesAudioName(Name, SoundPath, Owner != nullptr ? Owner->GetName() : FString(), Component->GetName())
			|| !IsNear(Component->GetComponentLocation()) || Components.Num() >= Limit)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("component"), AudioComponentText(*Component));
		Entry->SetStringField(TEXT("sound"), SoundPath);
		Entry->SetField(TEXT("owner"), Owner != nullptr ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Owner->GetName()))
			: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
		Entry->SetObjectField(TEXT("location"), PlayVectorJson(Component->GetComponentLocation()));
		Components.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Sounds started since the seq: Play Sound at Location / 2D (no component) and component sounds alike.
	FlushPendingLog();
	const TArray<FSoundStart> Started = FSoundStartRecord::Get().Query(World.GetUniqueID(), Since);
	TArray<const FSoundStart*> Matching;
	for (const FSoundStart& Entry : Started)
	{
		if (MatchesAudioName(Name, Entry.Sound, Entry.Owner, Entry.Component) && IsNear(Entry.Location))
		{
			Matching.Add(&Entry);
		}
	}
	const int32 Skipped = FMath::Max(0, Matching.Num() - Limit);
	TArray<TSharedPtr<FJsonValue>> Sounds;
	int32 Playing = 0;
	for (int32 Index = Skipped; Index < Matching.Num(); ++Index)
	{
		const FSoundStart& Entry = *Matching[Index];
		TSharedRef<FJsonObject> Sound = MakeShared<FJsonObject>();
		Sound->SetNumberField(TEXT("seq"), static_cast<double>(Entry.Seq));
		Sound->SetNumberField(TEXT("t"), RoundAudioTime(Entry.Started - SessionTime));
		Sound->SetStringField(TEXT("sound"), Entry.Sound);
		Sound->SetField(TEXT("owner"), Entry.Owner.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())
			: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Entry.Owner)));
		if (!Entry.Component.IsEmpty())
		{
			Sound->SetStringField(TEXT("component"), Entry.Component);
		}
		Sound->SetObjectField(TEXT("location"), PlayVectorJson(Entry.Location));
		const bool bFinished = Entry.Finished >= 0.0;
		Sound->SetBoolField(TEXT("finished"), bFinished);
		if (bFinished)
		{
			Sound->SetNumberField(TEXT("finished_t"), RoundAudioTime(Entry.Finished - SessionTime));
		}
		else
		{
			++Playing;
		}
		if (Entry.bVirtual)
		{
			// Started but inaudible (out of earshot, or over its concurrency limit): it plays silently.
			Sound->SetBoolField(TEXT("virtual"), true);
		}
		if (Entry.bUi)
		{
			Sound->SetBoolField(TEXT("ui"), true);
		}
		Sounds.Add(MakeShared<FJsonValueObject>(Sound));
	}

	TArray<FAssetDocumentDiagnostic> Warnings;
	const FAudioDeviceManager* Devices = GEngine != nullptr ? GEngine->GetAudioDeviceManager() : nullptr;
	if (Devices == nullptr || !World.GetAudioDevice().IsValid())
	{
		FAssetDocumentDiagnostic Warning = MakePlayDiagnostic(TEXT(""), TEXT("Play.AudioDisabled"),
			TEXT("The editor runs without audio (started with -nosound, or no audio device): no sound plays, so none is listed."));
		Warning.Severity = TEXT("warning");
		Warnings.Add(Warning);
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("world"), World.GetPathName());
	Payload->SetNumberField(TEXT("world_time"), FMath::RoundToDouble(World.GetTimeSeconds() * 100.0) / 100.0);
	Payload->SetNumberField(TEXT("since"), static_cast<double>(Since));
	Payload->SetNumberField(TEXT("last_seq"), static_cast<double>(FPlayLog::Get().GetLastSeq()));
	Payload->SetArrayField(TEXT("components"), Components);
	Payload->SetNumberField(TEXT("total"), Matching.Num());
	Payload->SetNumberField(TEXT("skipped"), Skipped);
	Payload->SetArrayField(TEXT("sounds"), Sounds);
	FLevelToolResult Result = FLevelToolResult::Success(FString::Printf(TEXT("%d audio components playing; %d sounds started%s (%d still playing)"),
		Components.Num(), Matching.Num(), Skipped > 0 ? *FString::Printf(TEXT(", the oldest %d left out"), Skipped) : TEXT(""), Playing), Payload);
	Result.Diagnostics = MoveTemp(Warnings);
	return Result;
}
}
