// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Play/PlayActions.h"
#include "Play/PlayCommandGuard.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentNameSuggestions.h"
#include "AssetDocumentSettings.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "LevelEditValues.h"
#include "Play/PlayQuery.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocumentLevel::Private::Play
{
// Named per file (unity builds merge the anonymous namespaces of the play tools).
namespace PlayActionsLocal
{
FString JsonText(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return TEXT("null");
	}
	FString Text;
	FJsonSerializer::Serialize(Value, FString(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	return Text.Left(300);
}

// A Play.ProtectedSettings refusal when a request value names a protected settings class (the plugin's own server and
// security settings, or engine settings that make the editor run code): a function argument (a console command for
// ServerExec) or a value to set (an object path to it).
bool RefuseProtectedSettingsValue(const TSharedPtr<FJsonValue>& Value, const FString& Path, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!Value.IsValid())
	{
		return false;
	}
	FString Text;
	FJsonSerializer::Serialize(Value, FString(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	const FString Why = UAssetDocumentSettings::WhyNamesProtectedSettings(Text);
	if (Why.IsEmpty())
	{
		return false;
	}
	Diagnostics.Add(MakePlayDiagnostic(Path, TEXT("Play.ProtectedSettings"), Why));
	return true;
}

bool CheckActionFields(const FJsonObject& Json, std::initializer_list<const TCHAR*> Known, const TCHAR* What, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	TArray<FString> Names;
	for (const TCHAR* Name : Known)
	{
		Names.Add(Name);
	}
	for (const auto& Field : Json.Values)
	{
		if (!Names.Contains(FString(*Field.Key)))
		{
			Diagnostics.Add(PlayInvalidRequest(TEXT("/") + FString(*Field.Key), FString::Printf(TEXT("Unknown %s field '%s'. Fields: %s."), What, *Field.Key,
				*FString::Join(Names, TEXT(", ")))));
		}
	}
	return Diagnostics.IsEmpty();
}

FAssetDocumentDiagnostic ActorDiagnostic(const FString& Path, const FPlayActorResolution& Resolution)
{
	TSharedPtr<FJsonObject> Details;
	if (!Resolution.Candidates.IsEmpty())
	{
		Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("candidates"), PlayStringArray(Resolution.Candidates));
	}
	return MakePlayDiagnostic(Path, Resolution.Code, Resolution.Message, Details);
}

AActor* ResolveTargetActor(UWorld& World, const FJsonObject& Request, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!Request.HasField(TEXT("actor")))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/actor"), TEXT("'actor' names the play-world actor: \"player\", \"game_mode\", an object name, label or path "
			"(query_play lists them).")));
		return nullptr;
	}
	const FPlayActorResolution Resolution = ResolvePlayActor(World, Request.TryGetField(TEXT("actor")));
	if (Resolution.Actor == nullptr)
	{
		Diagnostics.Add(ActorDiagnostic(TEXT("/actor"), Resolution));
	}
	return Resolution.Actor;
}

// A property by name or authored name (Blueprint variables, user struct members), case-insensitive.
FProperty* FindPropertyByName(const UStruct& Struct, const FString& Name)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (It->GetName().Equals(Name, ESearchCase::IgnoreCase) || It->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

// A string naming a play-world actor ("player", a name, label or path) or component ("Owner.Component") for an
// object property; anything else (an asset path) is left to the value codec.
bool ResolvePlayObject(UWorld& World, const FObjectPropertyBase& Property, const TSharedPtr<FJsonValue>& Json, UObject*& OutObject,
	bool& bOutHandled, FString& OutError)
{
	bOutHandled = false;
	if (!Json.IsValid() || Property.PropertyClass == nullptr)
	{
		return true;
	}
	if (Json->IsNull())
	{
		OutObject = nullptr;
		bOutHandled = true;
		return true;
	}
	FString Text;
	if (!Json->TryGetString(Text) || Text.IsEmpty())
	{
		return true;
	}
	UClass* Wanted = Property.PropertyClass;
	if (Wanted->IsChildOf<AActor>() || AActor::StaticClass()->IsChildOf(Wanted))
	{
		const FPlayActorResolution Resolution = ResolvePlayActor(World, Json);
		if (Resolution.Actor != nullptr)
		{
			if (!Resolution.Actor->IsA(Wanted))
			{
				OutError = FString::Printf(TEXT("'%s' is a %s, not a %s"), *Text, *Resolution.Actor->GetClass()->GetName(), *Wanted->GetName());
				return false;
			}
			OutObject = Resolution.Actor;
			bOutHandled = true;
			return true;
		}
	}
	FString OwnerName;
	FString ComponentName;
	if ((Wanted->IsChildOf<UActorComponent>() || UActorComponent::StaticClass()->IsChildOf(Wanted)) && !Text.StartsWith(TEXT("/"))
		&& Text.Split(TEXT("."), &OwnerName, &ComponentName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		const FPlayActorResolution Owner = ResolvePlayActor(World, MakeShared<FJsonValueString>(OwnerName));
		UActorComponent* Component = Owner.Actor != nullptr ? FindPlayComponent(*Owner.Actor, ComponentName) : nullptr;
		if (Component != nullptr)
		{
			if (!Component->IsA(Wanted))
			{
				OutError = FString::Printf(TEXT("'%s' is a %s, not a %s"), *Text, *Component->GetClass()->GetName(), *Wanted->GetName());
				return false;
			}
			OutObject = Component;
			bOutHandled = true;
		}
	}
	return true;
}

// Json written into the property value at ValuePtr (an edit of the value there: struct members left out keep it).
bool DecodeInto(UWorld& World, FProperty& Property, const TSharedPtr<FJsonValue>& Json, void* ValuePtr, FString& OutError)
{
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
	{
		UObject* Object = nullptr;
		bool bHandled = false;
		if (!ResolvePlayObject(World, *ObjectProperty, Json, Object, bHandled, OutError))
		{
			return false;
		}
		if (bHandled)
		{
			ObjectProperty->SetObjectPropertyValue(ValuePtr, Object);
			return true;
		}
	}
	const FLevelValue Value = JsonToLevelValue(Json);
	FString Code;
	if (!UE::AssetDocument::EditorPropertyValueCodec::Decode(Property, Value, ValuePtr, Code))
	{
		OutError = FString::Printf(TEXT("%s (%s) does not take %s%s"), *Property.GetAuthoredName(), *Property.GetCPPType(), *JsonText(Json),
			Code.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Code));
		return false;
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
	{
		if (const FString Why = UAssetDocumentSettings::WhyWriteProtected(ObjectProperty->GetObjectPropertyValue(ValuePtr)); !Why.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s cannot hold a protected settings object. %s"), *Property.GetAuthoredName(), *Why);
			return false;
		}
	}
	return true;
}

FString DescribeParameter(const FProperty& Property)
{
	return FString::Printf(TEXT("%s (%s)"), *Property.GetAuthoredName(), *Property.GetCPPType());
}

bool IsCallable(const UFunction& Function, FString& OutWhy)
{
	if (Function.HasAnyFunctionFlags(FUNC_Delegate))
	{
		OutWhy = TEXT("it is a delegate signature, not a function");
		return false;
	}
	if (Function.GetName().StartsWith(TEXT("ExecuteUbergraph")))
	{
		OutWhy = TEXT("it is the Blueprint's internal event graph; call one of its events instead");
		return false;
	}
#if WITH_EDITORONLY_DATA
	if (Function.HasMetaData(TEXT("Latent")))
	{
		OutWhy = TEXT("it is a latent action (like Delay) that needs a running Blueprint graph; call a function or custom event that starts it");
		return false;
	}
#endif
	const UClass* Owner = Function.GetOuterUClass();
	if (!Function.HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Exec) && (Owner == nullptr || Owner->ClassGeneratedBy == nullptr))
	{
		OutWhy = TEXT("it is not exposed to Blueprints (a Blueprint could not call it either)");
		return false;
	}
	return true;
}

UFunction* FindFunctionByAnyName(const UClass& Class, const FString& Name)
{
	if (UFunction* Function = Class.FindFunctionByName(FName(*Name)))
	{
		return Function;
	}
	for (TFieldIterator<UFunction> It(&Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
#if WITH_EDITOR
		if (It->GetDisplayNameText().ToString().Equals(Name, ESearchCase::IgnoreCase))
		{
			return *It;
		}
#endif
	}
	return nullptr;
}

TArray<FString> CloseFunctions(const UClass& Class, const FString& Name)
{
	TArray<FString> Names;
	for (TFieldIterator<UFunction> It(&Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FString Why;
		if (IsCallable(**It, Why))
		{
			Names.AddUnique(It->GetName());
		}
	}
	return UE::AssetDocument::SuggestCloseNames(Name, Names, 6);
}

// A writable value of a play-world actor by path, as ReadActorValue reads it.
struct FWritableValue
{
	FString Path;
	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	void* Staged = nullptr;
};

// A value of Actor by path; false with OutError and OutCode (Play.PropertyNotFound, Play.ProtectedSettings, or
// Play.TargetOutsidePlay for a path through an object reference that leaves the play session).
bool ResolveWritable(AActor& Actor, const FString& Path, FWritableValue& Out, FString& OutError, FString& OutCode)
{
	OutCode = TEXT("Play.PropertyNotFound");
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."));
	if (Segments.IsEmpty())
	{
		OutError = TEXT("empty property path");
		return false;
	}
	const UStruct* Struct = Actor.GetClass();
	void* Container = &Actor;
	int32 Index = 0;
	if (FindPropertyByName(*Struct, Segments[0]) == nullptr)
	{
		if (UActorComponent* Component = FindPlayComponent(Actor, Segments[0]))
		{
			if (Segments.Num() == 1)
			{
				OutError = FString::Printf(TEXT("'%s' is a component; name one of its properties (\"%s.<Property>\")"), *Segments[0], *Segments[0]);
				return false;
			}
			Struct = Component->GetClass();
			Container = Component;
			Index = 1;
		}
	}
	for (; Index < Segments.Num(); ++Index)
	{
		FProperty* Property = FindPropertyByName(*Struct, Segments[Index]);
		if (Property == nullptr)
		{
			OutError = FString::Printf(TEXT("'%s' has no property or component '%s'"), *Struct->GetName(), *Segments[Index]);
			return false;
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
		if (Index == Segments.Num() - 1)
		{
			Out.Path = Path;
			Out.Property = Property;
			Out.ValuePtr = ValuePtr;
			return true;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			Struct = StructProperty->Struct;
			Container = ValuePtr;
			continue;
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* Next = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			if (Next == nullptr)
			{
				OutError = FString::Printf(TEXT("'%s' is None; '%s' cannot be set inside it"), *Segments[Index], *Segments[Index + 1]);
				return false;
			}
			if (const FString Why = UAssetDocumentSettings::WhyWriteProtected(Next); !Why.IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' holds a protected settings object, whose values cannot be set. %s"), *Segments[Index], *Why);
				OutCode = TEXT("Play.ProtectedSettings");
				return false;
			}
			// Play changes the play session only: an object reference to an asset, a class default or an editor object
			// is not followed.
			if (!IsPlaySessionObject(*Next, Actor.GetWorld()))
			{
				OutError = FString::Printf(TEXT("'%s' holds %s, which is outside the play session; play sets values of play-world objects "
					"only"), *Segments[Index], *Next->GetPathName());
				OutCode = TEXT("Play.TargetOutsidePlay");
				return false;
			}
			Struct = Next->GetClass();
			Container = Next;
			continue;
		}
		OutError = FString::Printf(TEXT("'%s' is not a struct or object; '%s' cannot be set inside it"), *Segments[Index], *Segments[Index + 1]);
		return false;
	}
	return false;
}

void FreeStaged(TArray<FWritableValue>& Values)
{
	for (FWritableValue& Value : Values)
	{
		if (Value.Staged != nullptr)
		{
			Value.Property->DestroyValue(Value.Staged);
			FMemory::Free(Value.Staged);
			Value.Staged = nullptr;
		}
	}
}

bool ReadVectorMembers(const TSharedPtr<FJsonValue>& Json, const TCHAR* const (&Members)[3], FVector& InOut)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Json.IsValid() || !Json->TryGetObject(Object) || Object == nullptr)
	{
		return false;
	}
	for (const auto& Member : (*Object)->Values)
	{
		double Number = 0.0;
		int32 Slot = INDEX_NONE;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Slot = FString(*Member.Key).Equals(Members[Index], ESearchCase::IgnoreCase) ? Index : Slot;
		}
		if (Slot == INDEX_NONE || !Member.Value.IsValid() || !Member.Value->TryGetNumber(Number))
		{
			return false;
		}
		InOut[Slot] = Number;
	}
	return true;
}

TSharedRef<FJsonObject> RotationJson(const FRotator& Rotation)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("Pitch"), FMath::RoundToDouble(Rotation.Pitch * 100.0) / 100.0);
	Json->SetNumberField(TEXT("Yaw"), FMath::RoundToDouble(Rotation.Yaw * 100.0) / 100.0);
	Json->SetNumberField(TEXT("Roll"), FMath::RoundToDouble(Rotation.Roll * 100.0) / 100.0);
	return Json;
}

TSharedRef<FJsonObject> ChangeJson(const FString& Path, const TSharedPtr<FJsonValue>& Before, const TSharedPtr<FJsonValue>& After)
{
	TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
	Change->SetStringField(TEXT("path"), Path);
	Change->SetField(TEXT("before"), Before.IsValid() ? Before : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
	Change->SetField(TEXT("after"), After.IsValid() ? After : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
	return Change;
}
} // namespace PlayActionsLocal

FLevelToolResult RunPlayCall(UWorld& World, const FJsonObject& Request)
{
	using namespace PlayActionsLocal;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	CheckActionFields(Request, {TEXT("action"), TEXT("actor"), TEXT("function"), TEXT("args")}, TEXT("play call"), Diagnostics);
	FString FunctionText;
	if (!Request.TryGetStringField(TEXT("function"), FunctionText) || FunctionText.IsEmpty())
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/function"), TEXT("'function' names a Blueprint function, custom event or UFUNCTION of the actor "
			"(\"SetAutoAttack\"), or of one of its components (\"CharacterMovement.StopMovementImmediately\").")));
	}
	AActor* Actor = ResolveTargetActor(World, Request, Diagnostics);
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	UObject* Target = Actor;
	FString FunctionName = FunctionText;
	FString ComponentName;
	if (FunctionText.Split(TEXT("."), &ComponentName, &FunctionName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		Target = FindPlayComponent(*Actor, ComponentName);
		if (Target == nullptr)
		{
			TArray<FString> Names;
			TInlineComponentArray<UActorComponent*> Components(Actor);
			for (const UActorComponent* Component : Components)
			{
				Names.Add(Component->GetName());
			}
			Diagnostics.Add(MakePlayDiagnostic(TEXT("/function"), TEXT("Play.ComponentNotFound"), FString::Printf(
				TEXT("%s has no component '%s'. Components: %s."), *Actor->GetName(), *ComponentName, *FString::Join(Names, TEXT(", ")))));
			return FLevelToolResult::Failure(MoveTemp(Diagnostics));
		}
	}
	UFunction* Function = FindFunctionByAnyName(*Target->GetClass(), FunctionName);
	if (Function == nullptr)
	{
		const TArray<FString> Close = CloseFunctions(*Target->GetClass(), FunctionName);
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/function"), TEXT("Play.FunctionNotFound"), FString::Printf(TEXT("%s (%s) has no function '%s'.%s"),
			*Target->GetName(), *Target->GetClass()->GetName(), *FunctionName,
			Close.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", "))))));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	FString Why;
	if (!IsCallable(*Function, Why))
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/function"), TEXT("Play.FunctionNotCallable"), FString::Printf(TEXT("%s cannot be called: %s."),
			*Function->GetName(), *Why)));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	// Functions that run console commands or Python are not called, whatever their arguments; arguments naming the
	// plugin's settings are reported as well.
	if (const FString Refused = RefusedFunction(*Function); !Refused.IsEmpty())
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/function"), TEXT("Play.FunctionNotCallable"), FString::Printf(TEXT("%s cannot be called here: %s."),
			*Function->GetName(), *Refused)));
		RefuseProtectedSettingsValue(Request.TryGetField(TEXT("args")), TEXT("/args"), Diagnostics);
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	// Parameters: inputs (by value or by reference), outputs, the return value.
	TArray<FProperty*> Inputs;
	TArray<FProperty*> Outputs;
	FProperty* ReturnProperty = nullptr;
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			ReturnProperty = *It;
		}
		else if (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm))
		{
			Outputs.Add(*It);
		}
		else
		{
			Inputs.Add(*It);
			if (It->HasAnyPropertyFlags(CPF_OutParm))
			{
				Outputs.Add(*It);
			}
		}
	}
	FString WorldContextName;
#if WITH_EDITORONLY_DATA
	WorldContextName = Function->GetMetaData(TEXT("WorldContext"));
#endif
	TArray<FProperty*> Positional = Inputs.FilterByPredicate([&WorldContextName](const FProperty* Property)
	{
		return WorldContextName.IsEmpty() || !Property->GetName().Equals(WorldContextName);
	});
	TArray<FString> InputNames;
	for (const FProperty* Property : Positional)
	{
		InputNames.Add(DescribeParameter(*Property));
	}
	const FString Signature = FString::Printf(TEXT("%s(%s)"), *Function->GetName(), *FString::Join(InputNames, TEXT(", ")));

	TMap<FProperty*, TSharedPtr<FJsonValue>> Given;
	if (const TSharedPtr<FJsonValue> Args = Request.TryGetField(TEXT("args")); RefuseProtectedSettingsValue(Args, TEXT("/args"), Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (const TSharedPtr<FJsonValue> Args = Request.TryGetField(TEXT("args")))
	{
		if (Args->Type == EJson::Object)
		{
			for (const auto& Arg : Args->AsObject()->Values)
			{
				FProperty* const* Found = Inputs.FindByPredicate([&Arg](const FProperty* Property)
				{
					return Property->GetName().Equals(FString(*Arg.Key), ESearchCase::IgnoreCase) || Property->GetAuthoredName().Equals(FString(*Arg.Key), ESearchCase::IgnoreCase);
				});
				if (Found == nullptr)
				{
					const bool bOutput = Outputs.ContainsByPredicate([&Arg](const FProperty* Property)
					{
						return Property->GetAuthoredName().Equals(FString(*Arg.Key), ESearchCase::IgnoreCase);
					});
					Diagnostics.Add(PlayInvalidRequest(TEXT("/args/") + FString(*Arg.Key), FString::Printf(TEXT("%s %s. Parameters: %s."), *Signature,
						bOutput ? *FString::Printf(TEXT("returns '%s' (an output, read it from the result)"), *Arg.Key)
							: *FString::Printf(TEXT("has no parameter '%s'"), *Arg.Key),
						InputNames.IsEmpty() ? TEXT("none") : *FString::Join(InputNames, TEXT(", ")))));
					continue;
				}
				Given.Add(*Found, Arg.Value);
			}
		}
		else if (Args->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = Args->AsArray();
			if (Values.Num() > Positional.Num())
			{
				Diagnostics.Add(PlayInvalidRequest(TEXT("/args"), FString::Printf(TEXT("%d arguments for %s."), Values.Num(), *Signature)));
			}
			for (int32 Index = 0; Index < Values.Num() && Index < Positional.Num(); ++Index)
			{
				Given.Add(Positional[Index], Values[Index]);
			}
		}
		else
		{
			Diagnostics.Add(PlayInvalidRequest(TEXT("/args"), FString::Printf(TEXT("'args' is an object {\"Parameter\": value} or an array in "
				"parameter order: %s."), *Signature)));
		}
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	FStructOnScope Parameters(Function);
	uint8* Memory = Parameters.GetStructMemory();
	TArray<FString> Defaulted;
	for (FProperty* Property : Inputs)
	{
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Memory);
		if (const TSharedPtr<FJsonValue>* Value = Given.Find(Property))
		{
			FString Error;
			if (!DecodeInto(World, *Property, *Value, ValuePtr, Error))
			{
				Diagnostics.Add(MakePlayDiagnostic(TEXT("/args/") + Property->GetAuthoredName(), TEXT("Play.InvalidArgument"),
					FString::Printf(TEXT("%s: %s."), *Signature, *Error)));
			}
			continue;
		}
		if (!WorldContextName.IsEmpty() && Property->GetName().Equals(WorldContextName))
		{
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				ObjectProperty->SetObjectPropertyValue(ValuePtr, Target);
			}
			continue;
		}
		// The default a Blueprint node shows for the pin (UFUNCTION defaults, Blueprint function input defaults).
		FString Default;
		if (UEdGraphSchema_K2::FindFunctionParameterDefaultValue(Function, Property, Default) && !Default.IsEmpty())
		{
			FBlueprintEditorUtils::PropertyValueFromString_Direct(Property, Default, static_cast<uint8*>(ValuePtr), Target);
		}
		Defaulted.Add(Property->GetAuthoredName());
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	{
		FPlayWorldScope PlayWorld(World);
		Target->ProcessEvent(Function, Memory);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("target"), Target == Actor ? Actor->GetName() : Actor->GetName() + TEXT(".") + Target->GetName());
	Payload->SetStringField(TEXT("function"), Function->GetPathName());
	if (ReturnProperty != nullptr)
	{
		Payload->SetField(TEXT("return"), EncodePlayValue(*ReturnProperty, ReturnProperty->ContainerPtrToValuePtr<void>(Memory)));
	}
	if (!Outputs.IsEmpty())
	{
		TSharedRef<FJsonObject> OutputJson = MakeShared<FJsonObject>();
		for (FProperty* Property : Outputs)
		{
			OutputJson->SetField(Property->GetAuthoredName(), EncodePlayValue(*Property, Property->ContainerPtrToValuePtr<void>(Memory)));
		}
		Payload->SetObjectField(TEXT("outputs"), OutputJson);
	}
	if (!Defaulted.IsEmpty())
	{
		Payload->SetArrayField(TEXT("defaulted"), PlayStringArray(Defaulted));
	}
	return FLevelToolResult::Success(FString::Printf(TEXT("Called %s on %s."), *Function->GetName(), *Payload->GetStringField(TEXT("target"))), Payload);
}

FLevelToolResult RunPlaySet(UWorld& World, const FJsonObject& Request)
{
	using namespace PlayActionsLocal;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	CheckActionFields(Request, {TEXT("action"), TEXT("actor"), TEXT("values"), TEXT("location"), TEXT("rotation")}, TEXT("play set"), Diagnostics);
	AActor* Actor = ResolveTargetActor(World, Request, Diagnostics);
	const TSharedPtr<FJsonObject>* Values = nullptr;
	if (Request.HasField(TEXT("values")) && (!Request.TryGetObjectField(TEXT("values"), Values) || Values == nullptr))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/values"), TEXT("'values' is an object {\"<path>\": value}: \"Health\", \"BPC_Combat.bAutoAttack\", "
			"\"CharacterMovement.Velocity\".")));
	}
	if (!Request.HasField(TEXT("values")) && !Request.HasField(TEXT("location")) && !Request.HasField(TEXT("rotation")))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT(""), TEXT("set needs 'values', 'location' and/or 'rotation'.")));
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	FVector Location = Actor->GetActorLocation();
	FVector RotationVector(Actor->GetActorRotation().Pitch, Actor->GetActorRotation().Yaw, Actor->GetActorRotation().Roll);
	static const TCHAR* const LocationMembers[3] = {TEXT("X"), TEXT("Y"), TEXT("Z")};
	static const TCHAR* const RotationMembers[3] = {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")};
	const bool bMove = Request.HasField(TEXT("location")) || Request.HasField(TEXT("rotation"));
	if (Request.HasField(TEXT("location")) && !ReadVectorMembers(Request.TryGetField(TEXT("location")), LocationMembers, Location))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/location"), TEXT("'location' is {\"X\", \"Y\", \"Z\"} in world cm (members left out keep their value).")));
	}
	if (Request.HasField(TEXT("rotation")) && !ReadVectorMembers(Request.TryGetField(TEXT("rotation")), RotationMembers, RotationVector))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/rotation"), TEXT("'rotation' is {\"Pitch\", \"Yaw\", \"Roll\"} in degrees (members left out keep their value).")));
	}
	if (bMove && Actor->GetRootComponent() == nullptr)
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/location"), TEXT("Play.InvalidRequest"), FString::Printf(TEXT("%s has no root component to move."), *Actor->GetName())));
	}

	// Every value is resolved and decoded into a staged copy before anything is written.
	TArray<FWritableValue> Writes;
	if (Values != nullptr)
	{
		for (const auto& Entry : (*Values)->Values)
		{
			const FString Path = TEXT("/values/") + FString(*Entry.Key);
			if (RefuseProtectedSettingsValue(Entry.Value, Path, Diagnostics))
			{
				continue;
			}
			FWritableValue Write;
			FString Error;
			FString Code;
			if (!ResolveWritable(*Actor, FString(*Entry.Key), Write, Error, Code))
			{
				Diagnostics.Add(MakePlayDiagnostic(Path, Code, Error.EndsWith(TEXT(".")) ? Error : Error + TEXT(".")));
				continue;
			}
			Write.Staged = FMemory::Malloc(Write.Property->GetSize(), Write.Property->GetMinAlignment());
			Write.Property->InitializeValue(Write.Staged);
			Write.Property->CopyCompleteValue(Write.Staged, Write.ValuePtr);
			Writes.Add(Write);
			if (!DecodeInto(World, *Write.Property, Entry.Value, Write.Staged, Error))
			{
				Diagnostics.Add(MakePlayDiagnostic(Path, TEXT("Play.InvalidValue"), Error + TEXT(".")));
			}
		}
	}
	if (!Diagnostics.IsEmpty())
	{
		FreeStaged(Writes);
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	TArray<TSharedPtr<FJsonValue>> Changes;
	TArray<FString> Paths;
	for (FWritableValue& Write : Writes)
	{
		const TSharedPtr<FJsonValue> Before = EncodePlayValue(*Write.Property, Write.ValuePtr);
		Write.Property->CopyCompleteValue(Write.ValuePtr, Write.Staged);
		Changes.Add(MakeShared<FJsonValueObject>(ChangeJson(Write.Path, Before, EncodePlayValue(*Write.Property, Write.ValuePtr))));
		Paths.Add(Write.Path);
	}
	FreeStaged(Writes);
	TArray<FAssetDocumentDiagnostic> Warnings;
	if (bMove)
	{
		const FVector BeforeLocation = Actor->GetActorLocation();
		const FRotator BeforeRotation = Actor->GetActorRotation();
		const FRotator Rotation(RotationVector.X, RotationVector.Y, RotationVector.Z);
		Actor->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
		if (Request.HasField(TEXT("location")))
		{
			Changes.Add(MakeShared<FJsonValueObject>(ChangeJson(TEXT("location"), MakeShared<FJsonValueObject>(PlayVectorJson(BeforeLocation)),
				MakeShared<FJsonValueObject>(PlayVectorJson(Actor->GetActorLocation())))));
			if (!Actor->GetActorLocation().Equals(Location, 0.1))
			{
				FAssetDocumentDiagnostic Warning = MakePlayDiagnostic(TEXT("/location"), TEXT("Play.NotMoved"), FString::Printf(
					TEXT("%s did not move to the location (its root component is not Movable, or the game moved it back)."), *Actor->GetName()));
				Warning.Severity = TEXT("warning");
				Warnings.Add(Warning);
			}
		}
		if (Request.HasField(TEXT("rotation")))
		{
			Changes.Add(MakeShared<FJsonValueObject>(ChangeJson(TEXT("rotation"), MakeShared<FJsonValueObject>(RotationJson(BeforeRotation)),
				MakeShared<FJsonValueObject>(RotationJson(Actor->GetActorRotation())))));
		}
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("changes"), Changes);
	FPlayActorJsonOptions Options;
	Options.Properties = Paths;
	Payload->SetObjectField(TEXT("actor"), PlayActorJson(*Actor, Options));
	FLevelToolResult Result = FLevelToolResult::Success(FString::Printf(TEXT("Set %d value(s) on %s."), Changes.Num(), *Actor->GetName()), Payload);
	Result.Diagnostics = MoveTemp(Warnings);
	return Result;
}
}
