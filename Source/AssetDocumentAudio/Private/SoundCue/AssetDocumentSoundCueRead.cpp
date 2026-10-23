// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// audio.soundcue.read.v1 (read-only): the facts of a SoundCue the profile needs besides its graph (which it reads with
// graph.query.v1):
//   {schema_version, defaults?: [{class, inputs?}]} ->
//   {editable, open_in_editor, status, messages[{severity, message, node?, path?}], derived {Duration, MaxDistance,
//    TotalSamples, bHasDelayNode, bHasConcatenatorNode, bHasPlayWhenSilent}, quality_levels [names],
//    waves [{node, path, duration?, looping}],
//    defaults [{class, inputs, starting_inputs, min, max, properties}] (one per requested class: the Details values of
//    a new node of the class with `inputs` child slots, default: its starting connectors), comment_defaults}.
// Reading never changes the cue.

#include "AssetDocumentAudioPrimitives.h"

#include "Dom/JsonObject.h"
#include "Sound/AudioSettings.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "SoundCue/AssetDocumentSoundCue.h"

namespace UE::AssetDocument::Graph::Audio
{
namespace
{
FValue ReflectedValue(const UObject& Object, const TCHAR* Name)
{
	const FProperty* Property = Object.GetClass()->FindPropertyByName(Name);
	return Property != nullptr ? EncodeAssetDocumentGraphPropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(&Object)) : MakeNull();
}

constexpr int32 MaxDefaultsPerCall = 16;
} // namespace

bool HandleSoundCueRead(FCall& Call, FObjectBuilder& Result)
{
	using namespace SoundCue;
	const FValue* Requested = nullptr;
	if (!Call.Args.OnlyFields({TEXT("defaults")}) || !Call.Args.Array(TEXT("defaults"), Requested, false))
	{
		return false;
	}
	const USoundCue* Cue = Cast<USoundCue>(&Call.Asset);
	if (Cue == nullptr)
	{
		return Fail(Call, TEXT("SoundCue.UnsupportedAsset"), TEXT("audio.soundcue.read.v1 reads SoundCues."), TEXT("/HandleArguments/asset"));
	}
	if (Requested != nullptr && Requested->Elements.Num() > MaxDefaultsPerCall)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("At most %d defaults rows per call."), MaxDefaultsPerCall), TEXT("defaults"));
	}
	Result.AddBool(TEXT("editable"), IsEditableCue(*Cue));
	Result.AddBool(TEXT("open_in_editor"), IsOpenInEditor(*Cue));

	const TSharedPtr<FJsonObject> Status = DescribeCompileStatus(*Cue);
	TArray<FValue> Messages;
	if (Status.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& Row : Status->GetArrayField(TEXT("Messages")))
		{
			const TSharedPtr<FJsonObject> Object = Row->AsObject();
			FObjectBuilder Message;
			Message.AddString(TEXT("severity"), Object->GetStringField(TEXT("Severity")).ToLower());
			Message.AddString(TEXT("message"), Object->GetStringField(TEXT("Message")));
			FString Text;
			if (Object->TryGetStringField(TEXT("Node"), Text))
			{
				Message.AddString(TEXT("node"), Text);
			}
			if (Object->TryGetStringField(TEXT("Path"), Text))
			{
				Message.AddString(TEXT("path"), Text);
			}
			Messages.Add(Message.Build());
		}
	}
	Result.AddString(TEXT("status"), Status.IsValid() ? Status->GetStringField(TEXT("Status")) : FString(TEXT("Unknown")));
	Result.Add(TEXT("messages"), MakeArray(MoveTemp(Messages)));

	FObjectBuilder Derived;
	for (const TCHAR* Name : {TEXT("Duration"), TEXT("MaxDistance"), TEXT("TotalSamples"), TEXT("bHasDelayNode"),
		TEXT("bHasConcatenatorNode"), TEXT("bHasPlayWhenSilent")})
	{
		Derived.Add(Name, ReflectedValue(*Cue, Name));
	}
	Result.Add(TEXT("derived"), Derived.Build());

	TArray<FString> Levels;
	const UAudioSettings* Settings = GetDefault<UAudioSettings>();
	for (int32 Index = 0; Index < Settings->GetQualityLevelSettingsNum(); ++Index)
	{
		Levels.Add(Settings->GetQualityLevelSettings(Index).DisplayName.ToString());
	}
	Result.Add(TEXT("quality_levels"), MakeStringArray(Levels));

	TArray<FValue> Waves;
	for (const USoundNode* Node : Cue->AllNodes)
	{
		const USoundNodeWavePlayer* Player = Cast<USoundNodeWavePlayer>(Node);
		if (Player == nullptr)
		{
			continue;
		}
		const FValue Path = ReflectedValue(*Player, TEXT("SoundWaveAssetPtr"));
		FObjectBuilder Row;
		Row.AddString(TEXT("node"), Player->GetName());
		Row.Add(TEXT("path"), Path);
		if (const USoundWave* Wave = Player->GetSoundWave())
		{
			Row.Add(TEXT("duration"), MakeCoordinate(Wave->Duration));
		}
		Row.AddBool(TEXT("looping"), Player->bLooping != 0);
		Waves.Add(Row.Build());
	}
	Result.Add(TEXT("waves"), MakeArray(MoveTemp(Waves)));

	if (Requested != nullptr)
	{
		TArray<FValue> Rows;
		for (int32 Index = 0; Index < Requested->Elements.Num(); ++Index)
		{
			const FString Key = FString::Printf(TEXT("defaults/%d"), Index);
			FReader Row = Call.Args.Child(Key, Requested->Elements[Index]);
			FString Label;
			int64 Inputs = -1;
			if (!Row.OnlyFields({TEXT("class"), TEXT("inputs")}) || !Row.String(TEXT("class"), Label) || !Row.Int(TEXT("inputs"), Inputs, false))
			{
				return false;
			}
			UClass* Class = ResolveClassLabel(Label);
			if (Class == nullptr || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				return Row.Fail(TEXT("SoundCue.UnknownNodeClass"), FString::Printf(TEXT("'%s' is not a sound node class."), *Label), TEXT("class"));
			}
			const int32 Starting = GetStartingInputs(*Class);
			const int32 Slots = Inputs >= 0 ? static_cast<int32>(FMath::Min<int64>(Inputs, USoundNode::MAX_ALLOWED_CHILD_NODES)) : Starting;
			Rows.Add(FObjectBuilder()
				.AddString(TEXT("class"), Label)
				.AddString(TEXT("class_path"), Class->GetPathName())
				.AddInt(TEXT("inputs"), Slots)
				.AddInt(TEXT("starting_inputs"), Starting)
				.AddInt(TEXT("min"), GetMinInputs(*Class))
				.AddInt(TEXT("max"), GetMaxInputs(*Class))
				.Add(TEXT("properties"), EncodeDefaultProperties(*Class, Slots))
				.Build());
		}
		Result.Add(TEXT("defaults"), MakeArray(MoveTemp(Rows)));
		Result.Add(TEXT("comment_defaults"), EncodeDefaultCommentProperties());
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Audio
