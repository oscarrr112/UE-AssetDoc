// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Play/PlayQuery.h"

#include "AIController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentTypes.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BrainComponent.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Camera/PlayerCameraManager.h"
#include "Misc/PackageName.h"
#include "Navigation/PathFollowingComponent.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Play/PlayAudio.h"
#include "Play/PlayInput.h"
#include "Play/PlayLog.h"
#include "Play/PlaySession.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocumentLevel::Private::Play
{
namespace
{
constexpr int32 DefaultLimit = 50;
constexpr int32 MaximumLimit = 500;
constexpr int32 MaxListedProperties = 200;
constexpr int32 MaxVitals = 12;

double Round2(double Value)
{
	return FMath::RoundToDouble(Value * 100.0) / 100.0;
}

FString RefText(const TSharedPtr<FJsonValue>& Ref)
{
	if (!Ref.IsValid())
	{
		return FString();
	}
	if (Ref->Type == EJson::String)
	{
		return Ref->AsString();
	}
	FString Text;
	FJsonSerializer::Serialize(Ref, FString(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	return Text;
}

bool IsListed(const AActor* Actor)
{
	return Actor != nullptr && IsValid(Actor) && !Actor->IsActorBeingDestroyed();
}

// A property by name or authored name (Blueprint variables, user struct members), case-insensitive.
FProperty* FindReadableProperty(const UStruct& Struct, const FString& Name)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}
		if (Property->GetName().Equals(Name, ESearchCase::IgnoreCase) || Property->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
		{
			return Property;
		}
	}
	return nullptr;
}

TArray<FString> CloseNames(const UStruct& Struct, const FString& Name)
{
	TArray<FString> Close;
	for (TFieldIterator<FProperty> It(&Struct); It && Close.Num() < 8; ++It)
	{
		const FString Authored = It->GetAuthoredName();
		if (Authored.Contains(Name, ESearchCase::IgnoreCase) || Name.Contains(Authored, ESearchCase::IgnoreCase))
		{
			Close.Add(Authored);
		}
	}
	return Close;
}

// Objects of the play world by name (an actor: its object name; a component: "Owner.Component").
FString DescribeObject(const UObject* Object)
{
	if (Object == nullptr)
	{
		return FString();
	}
	if (const AActor* Actor = Cast<AActor>(Object))
	{
		if (Actor->GetWorld() != nullptr && Actor->GetWorld()->IsPlayInEditor())
		{
			return Actor->GetName();
		}
	}
	if (const UActorComponent* Component = Cast<UActorComponent>(Object))
	{
		const AActor* Owner = Component->GetOwner();
		if (Owner != nullptr && Owner->GetWorld() != nullptr && Owner->GetWorld()->IsPlayInEditor())
		{
			return Owner->GetName() + TEXT(".") + Component->GetName();
		}
	}
	return Object->GetPathName();
}

TSharedPtr<FJsonValue> EncodeValue(FProperty& Property, const void* ValuePtr)
{
	return EncodePlayValue(Property, ValuePtr);
}

bool IsVisibleProperty(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) && !Property.HasAnyPropertyFlags(CPF_Deprecated);
}

bool IsVitalName(const FString& Name)
{
	static const TCHAR* const Tokens[] = {TEXT("health"), TEXT("hitpoint"), TEXT("dead"), TEXT("alive"), TEXT("stamina"),
		TEXT("mana"), TEXT("shield"), TEXT("armor"), TEXT("armour"), TEXT("lives")};
	for (const TCHAR* Token : Tokens)
	{
		if (Name.Contains(Token, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return Name.Equals(TEXT("HP"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("MaxHP"), ESearchCase::IgnoreCase);
}

void AddVitals(UObject& Object, const FString& Prefix, TSharedRef<FJsonObject>& Vitals)
{
	for (TFieldIterator<FProperty> It(Object.GetClass()); It && Vitals->Values.Num() < MaxVitals; ++It)
	{
		FProperty* Property = *It;
		if (!IsVisibleProperty(*Property) || Property->ArrayDim != 1
			|| !(Property->IsA<FNumericProperty>() || Property->IsA<FBoolProperty>()) || !IsVitalName(Property->GetAuthoredName()))
		{
			continue;
		}
		Vitals->SetField(Prefix + Property->GetAuthoredName(), EncodeValue(*Property, Property->ContainerPtrToValuePtr<void>(&Object)));
	}
}

FString MoveStatusName(EPathFollowingStatus::Type Status)
{
	switch (Status)
	{
	case EPathFollowingStatus::Waiting: return TEXT("Waiting");
	case EPathFollowingStatus::Paused: return TEXT("Paused");
	case EPathFollowingStatus::Moving: return TEXT("Moving");
	case EPathFollowingStatus::Idle:
	default: return TEXT("Idle");
	}
}

TSharedPtr<FJsonObject> AiJson(AAIController& Controller)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("controller"), Controller.GetName());
	Json->SetStringField(TEXT("controller_class"), Controller.GetClass()->GetPathName());
	UBrainComponent* Brain = Controller.GetBrainComponent();
	if (UBehaviorTreeComponent* Tree = Cast<UBehaviorTreeComponent>(Brain))
	{
		const UBehaviorTree* Current = Tree->GetCurrentTree();
		Json->SetStringField(TEXT("behavior_tree"), Current != nullptr ? Current->GetPathName() : FString());
		Json->SetBoolField(TEXT("running"), Tree->IsRunning());
		const UBTNode* Active = Tree->GetActiveNode();
		Json->SetStringField(TEXT("active_node"), Active != nullptr ? Active->GetNodeName() : FString());
		const FString Tasks = Tree->DescribeActiveTasks();
		if (!Tasks.IsEmpty())
		{
			Json->SetStringField(TEXT("active_tasks"), Tasks);
		}
	}
	else if (Brain != nullptr)
	{
		Json->SetStringField(TEXT("brain"), Brain->GetClass()->GetPathName());
		Json->SetBoolField(TEXT("running"), Brain->IsRunning());
	}
	if (const UBlackboardComponent* Blackboard = Controller.GetBlackboardComponent())
	{
		if (const UBlackboardData* Asset = Blackboard->GetBlackboardAsset())
		{
			Json->SetStringField(TEXT("blackboard_asset"), Asset->GetPathName());
			TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
			for (int32 Key = 0; Key < Asset->GetNumKeys(); ++Key)
			{
				const FBlackboard::FKey KeyId(Key);
				Values->SetStringField(Blackboard->GetKeyName(KeyId).ToString(),
					Blackboard->DescribeKeyValue(KeyId, EBlackboardDescription::OnlyValue));
			}
			Json->SetObjectField(TEXT("blackboard"), Values);
		}
	}
	Json->SetStringField(TEXT("move_status"), MoveStatusName(Controller.GetMoveStatus()));
	if (const AActor* Focus = Controller.GetFocusActor())
	{
		Json->SetStringField(TEXT("focus"), Focus->GetName());
	}
	return Json;
}

TSharedPtr<FJsonObject> MontageJson(AActor& Actor)
{
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(&Actor);
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		UAnimInstance* Anim = Mesh != nullptr ? Mesh->GetAnimInstance() : nullptr;
		UAnimMontage* Montage = Anim != nullptr ? Anim->GetCurrentActiveMontage() : nullptr;
		if (Montage == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("asset"), Montage->GetPathName());
		Json->SetNumberField(TEXT("position"), Round2(Anim->Montage_GetPosition(Montage)));
		Json->SetStringField(TEXT("section"), Anim->Montage_GetCurrentSection(Montage).ToString());
		Json->SetStringField(TEXT("component"), Mesh->GetName());
		return Json;
	}
	return nullptr;
}

TSharedRef<FJsonObject> RotatorJson(const FRotator& Rotation)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("Pitch"), Round2(Rotation.Pitch));
	Json->SetNumberField(TEXT("Yaw"), Round2(Rotation.Yaw));
	Json->SetNumberField(TEXT("Roll"), Round2(Rotation.Roll));
	return Json;
}

void AddChangedProperties(UObject& Object, const FString& Prefix, bool bDiffOnly, TSharedRef<FJsonObject>& Out)
{
	const UObject* Archetype = Object.GetArchetype();
	for (TFieldIterator<FProperty> It(Object.GetClass()); It && Out->Values.Num() < MaxListedProperties; ++It)
	{
		FProperty* Property = *It;
		if (!IsVisibleProperty(*Property))
		{
			continue;
		}
		const FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(Property);
		if (Reference != nullptr && Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance)
			&& Reference->PropertyClass != nullptr && Reference->PropertyClass->IsChildOf<UActorComponent>())
		{
			continue;
		}
		if (bDiffOnly && Archetype != nullptr && Archetype->GetClass() == Object.GetClass()
			&& Property->Identical_InContainer(&Object, Archetype, 0, PPF_None))
		{
			continue;
		}
		Out->SetField(Prefix + Property->GetAuthoredName(), EncodeValue(*Property, Property->ContainerPtrToValuePtr<void>(&Object)));
	}
}

UClass* ResolveActorClass(const FString& Path)
{
	UClass* Class = FindObject<UClass>(nullptr, *Path);
	if (Class == nullptr && FPackageName::IsValidObjectPath(Path))
	{
		Class = LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (Class == nullptr && !Path.EndsWith(TEXT("_C")))
	{
		FString ObjectPath = Path;
		if (!ObjectPath.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(ObjectPath))
		{
			ObjectPath += TEXT(".") + FPackageName::GetShortName(ObjectPath);
		}
		if (FPackageName::IsValidObjectPath(ObjectPath))
		{
			Class = FindObject<UClass>(nullptr, *(ObjectPath + TEXT("_C")));
			if (Class == nullptr)
			{
				Class = LoadObject<UClass>(nullptr, *(ObjectPath + TEXT("_C")), nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
		}
	}
	if (Class == nullptr && !Path.Contains(TEXT("/")) && !Path.Contains(TEXT(".")))
	{
		// A short name: a loaded actor class ("Character", "BP_Enemy", "BP_Enemy_C"), else a Blueprint asset of that name.
		const FString Generated = Path.EndsWith(TEXT("_C")) ? Path : Path + TEXT("_C");
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (!Candidate->IsChildOf<AActor>() || Candidate->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated)
				|| Candidate->GetName().StartsWith(TEXT("SKEL_")) || Candidate->GetName().StartsWith(TEXT("REINST_")))
			{
				continue;
			}
			if (Candidate->GetName().Equals(Path, ESearchCase::IgnoreCase) || Candidate->GetName().Equals(Generated, ESearchCase::IgnoreCase))
			{
				Class = Candidate;
				break;
			}
		}
		if (Class == nullptr)
		{
			TArray<FAssetData> Assets;
			IAssetRegistry::GetChecked().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets, true);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetName.ToString().Equals(Path, ESearchCase::IgnoreCase))
				{
					Class = LoadObject<UClass>(nullptr, *(Asset.GetObjectPathString() + TEXT("_C")), nullptr, LOAD_NoWarn | LOAD_Quiet);
					break;
				}
			}
		}
	}
	return Class != nullptr && Class->IsChildOf<AActor>() ? Class : nullptr;
}

struct FActorFilter
{
	UClass* Class = nullptr;
	bool bExactClass = false;
	FString Name;
	FString Tag;
	TOptional<FVector> NearLocation;
	double NearRadius = 0.0;
};

bool ParseActorFilter(UWorld& World, const FJsonObject& Json, const FString& Prefix, FActorFilter& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	FString ClassPath;
	if (Json.TryGetStringField(TEXT("class"), ClassPath))
	{
		Out.Class = ResolveActorClass(ClassPath);
		if (Out.Class == nullptr)
		{
			Diagnostics.Add(PlayInvalidRequest(Prefix + TEXT("/class"), FString::Printf(
				TEXT("'%s' is not an actor class (a class or Blueprint name such as Character or BP_Enemy, or a path)."), *ClassPath)));
		}
	}
	Json.TryGetBoolField(TEXT("exact_class"), Out.bExactClass);
	Json.TryGetStringField(TEXT("name"), Out.Name);
	Json.TryGetStringField(TEXT("tag"), Out.Tag);
	const TSharedPtr<FJsonObject>* Near = nullptr;
	if (Json.TryGetObjectField(TEXT("near"), Near) && Near != nullptr && Near->IsValid())
	{
		double Radius = 0.0;
		if (!(*Near)->TryGetNumberField(TEXT("radius"), Radius) || Radius < 0.0)
		{
			Diagnostics.Add(PlayInvalidRequest(Prefix + TEXT("/near/radius"), TEXT("'near' needs \"radius\" (cm, >= 0).")));
			return false;
		}
		Out.NearRadius = Radius;
		const TSharedPtr<FJsonObject>* Location = nullptr;
		if ((*Near)->TryGetObjectField(TEXT("location"), Location) && Location != nullptr && Location->IsValid())
		{
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			(*Location)->TryGetNumberField(TEXT("X"), X);
			(*Location)->TryGetNumberField(TEXT("Y"), Y);
			(*Location)->TryGetNumberField(TEXT("Z"), Z);
			Out.NearLocation = FVector(X, Y, Z);
		}
		else if ((*Near)->HasField(TEXT("actor")))
		{
			const FPlayActorResolution Center = ResolvePlayActor(World, (*Near)->TryGetField(TEXT("actor")));
			if (Center.Actor == nullptr)
			{
				Diagnostics.Add(MakePlayDiagnostic(Prefix + TEXT("/near/actor"), Center.Code, Center.Message));
				return false;
			}
			Out.NearLocation = Center.Actor->GetActorLocation();
		}
		else
		{
			Diagnostics.Add(PlayInvalidRequest(Prefix + TEXT("/near"), TEXT("'near' is {\"location\": {X,Y,Z} | \"actor\": <ref>, \"radius\": cm}.")));
			return false;
		}
	}
	return Diagnostics.IsEmpty();
}

bool MatchesFilter(AActor& Actor, const FActorFilter& Filter)
{
	if (Filter.Class != nullptr && (Filter.bExactClass ? Actor.GetClass() != Filter.Class : !Actor.IsA(Filter.Class)))
	{
		return false;
	}
	if (!Filter.Name.IsEmpty() && !Actor.GetName().MatchesWildcard(Filter.Name) && !Actor.GetActorLabel(false).MatchesWildcard(Filter.Name))
	{
		return false;
	}
	if (!Filter.Tag.IsEmpty() && !Actor.Tags.Contains(FName(*Filter.Tag)))
	{
		return false;
	}
	return !Filter.NearLocation.IsSet() || FVector::Dist(Actor.GetActorLocation(), Filter.NearLocation.GetValue()) <= Filter.NearRadius;
}

bool CompareJson(const TSharedPtr<FJsonValue>& Actual, const FString& Op, const TSharedPtr<FJsonValue>& Expected)
{
	const bool bActualNull = !Actual.IsValid() || Actual->IsNull();
	const bool bExpectedNull = !Expected.IsValid() || Expected->IsNull();
	if (bActualNull || bExpectedNull)
	{
		return Op == TEXT("==") ? bActualNull == bExpectedNull : Op == TEXT("!=") ? bActualNull != bExpectedNull : false;
	}
	double A = 0.0;
	double B = 0.0;
	if (Actual->Type == EJson::Number && Expected->Type == EJson::Number && Actual->TryGetNumber(A) && Expected->TryGetNumber(B))
	{
		if (Op == TEXT("==")) { return FMath::IsNearlyEqual(A, B, 1e-4); }
		if (Op == TEXT("!=")) { return !FMath::IsNearlyEqual(A, B, 1e-4); }
		if (Op == TEXT("<")) { return A < B; }
		if (Op == TEXT("<=")) { return A <= B; }
		if (Op == TEXT(">")) { return A > B; }
		if (Op == TEXT(">=")) { return A >= B; }
		return false;
	}
	bool bEqual = false;
	FString SA;
	FString SB;
	if (Actual->Type == EJson::String && Expected->Type == EJson::String && Actual->TryGetString(SA) && Expected->TryGetString(SB))
	{
		bEqual = SA.Equals(SB, ESearchCase::IgnoreCase);
	}
	else
	{
		bEqual = FJsonValue::CompareEqual(*Actual, *Expected);
	}
	return Op == TEXT("==") ? bEqual : Op == TEXT("!=") ? !bEqual : false;
}

const TCHAR* const QueryFields[] = {TEXT("what"), TEXT("actors"), TEXT("class"), TEXT("exact_class"), TEXT("name"), TEXT("tag"),
	TEXT("near"), TEXT("properties"), TEXT("detail"), TEXT("diff_only"), TEXT("limit"), TEXT("offset"), TEXT("since"),
	TEXT("category"), TEXT("verbosity"), TEXT("text"), TEXT("include_inactive")};

// The fields of each what; a field of another what is refused (it would be ignored).
const TCHAR* const ActorQueryFields = TEXT("actors, class, exact_class, name, tag, near, properties, detail, diff_only, limit, offset");
const TCHAR* const LogQueryFields = TEXT("since, category, verbosity, text, limit");
const TCHAR* const EffectQueryFields = TEXT("name, near, include_inactive, limit, offset");
const TCHAR* const AudioQueryFields = TEXT("since, name, near, limit");

bool IsQueryFieldOf(const FString& What, const FString& Field)
{
	if (Field == TEXT("what"))
	{
		return true;
	}
	if (What == TEXT("input"))
	{
		return false;
	}
	const TCHAR* const List = What == TEXT("log") ? LogQueryFields : What == TEXT("effects") ? EffectQueryFields
		: What == TEXT("audio") ? AudioQueryFields : ActorQueryFields;
	TArray<FString> Names;
	FString(List).ParseIntoArray(Names, TEXT(", "));
	return Names.Contains(Field);
}

FString EffectType(const UFXSystemComponent& Component)
{
	if (Component.IsA<UParticleSystemComponent>())
	{
		return TEXT("Cascade");
	}
	return Component.GetClass()->GetName().Contains(TEXT("Niagara")) ? FString(TEXT("Niagara")) : Component.GetClass()->GetPathName();
}

// query_play what "effects": the particle effect components of the play world (Niagara and Cascade), both those
// actors own and those spawned at a location (Spawn System at Location: owned by no gameplay actor).
FLevelToolResult QueryEffects(UWorld& World, const FJsonObject& Json)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	FActorFilter Filter;
	FJsonObject NearOnly;
	if (Json.HasField(TEXT("near")))
	{
		NearOnly.SetField(TEXT("near"), Json.TryGetField(TEXT("near")));
	}
	ParseActorFilter(World, NearOnly, FString(), Filter, Diagnostics);
	FString Name;
	Json.TryGetStringField(TEXT("name"), Name);
	bool bIncludeInactive = false;
	Json.TryGetBoolField(TEXT("include_inactive"), bIncludeInactive);
	int32 Limit = DefaultLimit;
	int32 Offset = 0;
	double Number = 0.0;
	if (Json.TryGetNumberField(TEXT("limit"), Number))
	{
		Limit = FMath::Clamp(static_cast<int32>(Number), 1, MaximumLimit);
	}
	if (Json.TryGetNumberField(TEXT("offset"), Number))
	{
		Offset = FMath::Max(0, static_cast<int32>(Number));
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	TArray<UFXSystemComponent*> Found;
	for (TObjectIterator<UFXSystemComponent> It; It; ++It)
	{
		UFXSystemComponent* Component = *It;
		if (!IsValid(Component) || Component->GetWorld() != &World || Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
			|| Component->IsBeingDestroyed() || (!bIncludeInactive && !Component->IsActive()))
		{
			continue;
		}
		const UFXSystemAsset* Asset = Component->GetFXSystemAsset();
		const AActor* Owner = Component->GetOwner();
		if (!Name.IsEmpty() && !Component->GetName().MatchesWildcard(Name) && !(Owner != nullptr && Owner->GetName().MatchesWildcard(Name))
			&& !(Asset != nullptr && (Asset->GetName().MatchesWildcard(Name) || Asset->GetPathName().MatchesWildcard(Name))))
		{
			continue;
		}
		if (Filter.NearLocation.IsSet() && FVector::Dist(Component->GetComponentLocation(), Filter.NearLocation.GetValue()) > Filter.NearRadius)
		{
			continue;
		}
		Found.Add(Component);
	}
	if (Filter.NearLocation.IsSet())
	{
		const FVector Center = Filter.NearLocation.GetValue();
		Found.Sort([&Center](const UFXSystemComponent& Left, const UFXSystemComponent& Right)
		{
			return FVector::DistSquared(Left.GetComponentLocation(), Center) < FVector::DistSquared(Right.GetComponentLocation(), Center);
		});
	}
	else
	{
		Found.Sort([](const UFXSystemComponent& Left, const UFXSystemComponent& Right) { return Left.GetPathName() < Right.GetPathName(); });
	}

	TArray<TSharedPtr<FJsonValue>> Effects;
	const int32 First = FMath::Min(Offset, Found.Num());
	const int32 Last = FMath::Min(Found.Num(), First + Limit);
	for (int32 Index = First; Index < Last; ++Index)
	{
		UFXSystemComponent& Component = *Found[Index];
		TSharedRef<FJsonObject> Effect = MakeShared<FJsonObject>();
		Effect->SetStringField(TEXT("component"), DescribeObject(&Component));
		Effect->SetStringField(TEXT("type"), EffectType(Component));
		const UFXSystemAsset* Asset = Component.GetFXSystemAsset();
		Effect->SetField(TEXT("asset"), Asset != nullptr ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetPathName()))
			: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
		const AActor* Owner = Component.GetOwner();
		Effect->SetField(TEXT("owner"), Owner != nullptr ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Owner->GetName()))
			: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
		const USceneComponent* Parent = Component.GetAttachParent();
		if (Parent != nullptr && Parent->GetOwner() != nullptr && Parent->GetOwner() != Owner)
		{
			Effect->SetStringField(TEXT("attached_to"), DescribeObject(Parent));
		}
		Effect->SetObjectField(TEXT("location"), PlayVectorJson(Component.GetComponentLocation()));
		Effect->SetBoolField(TEXT("active"), Component.IsActive());
		if (Filter.NearLocation.IsSet())
		{
			Effect->SetNumberField(TEXT("distance"), Round2(FVector::Dist(Component.GetComponentLocation(), Filter.NearLocation.GetValue())));
		}
		Effects.Add(MakeShared<FJsonValueObject>(Effect));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("world"), World.GetPathName());
	Payload->SetNumberField(TEXT("world_time"), Round2(World.GetTimeSeconds()));
	Payload->SetNumberField(TEXT("total"), Found.Num());
	Payload->SetNumberField(TEXT("offset"), Offset);
	Payload->SetNumberField(TEXT("limit"), Limit);
	Payload->SetArrayField(TEXT("effects"), Effects);
	return FLevelToolResult::Success(FString::Printf(TEXT("%d of %d %seffect components at world time %.2f s"), Effects.Num(), Found.Num(),
		bIncludeInactive ? TEXT("") : TEXT("active "), World.GetTimeSeconds()), Payload);
}

FLevelToolResult QueryLog(const FJsonObject& Json)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	uint64 SessionSeq = 0;
	double SessionTime = 0.0;
	GetSessionLogBase(SessionSeq, SessionTime);
	FPlayLogFilter Filter;
	Filter.Since = SessionSeq;
	double Since = 0.0;
	FString SinceText;
	if (Json.TryGetNumberField(TEXT("since"), Since))
	{
		Filter.Since = Since < 0.0 ? 0 : static_cast<uint64>(Since);
	}
	else if (Json.TryGetStringField(TEXT("since"), SinceText))
	{
		if (SinceText == TEXT("all"))
		{
			Filter.Since = 0;
		}
		else if (SinceText != TEXT("session"))
		{
			Diagnostics.Add(PlayInvalidRequest(TEXT("/since"), TEXT("'since' is a log seq number, \"session\" (default) or \"all\".")));
		}
	}
	FString Category;
	if (Json.TryGetStringField(TEXT("category"), Category) && !Category.IsEmpty())
	{
		Category.ParseIntoArray(Filter.Categories, TEXT(","));
		for (FString& Entry : Filter.Categories)
		{
			Entry.TrimStartAndEndInline();
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Categories = nullptr;
	if (Json.TryGetArrayField(TEXT("category"), Categories) && Categories != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *Categories)
		{
			Filter.Categories.Add(Entry.IsValid() ? Entry->AsString() : FString());
		}
	}
	FString Verbosity;
	if (Json.TryGetStringField(TEXT("verbosity"), Verbosity) && !ParseVerbosity(Verbosity, Filter.MaxVerbosity))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/verbosity"), TEXT("'verbosity' is \"error\", \"warning\", \"display\", \"log\" (default) or \"verbose\".")));
	}
	Json.TryGetStringField(TEXT("text"), Filter.Text);
	int32 Limit = DefaultLimit;
	double LimitNumber = 0.0;
	if (Json.TryGetNumberField(TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, MaximumLimit);
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	FlushPendingLog();
	int32 Skipped = 0;
	uint64 OldestKept = 0;
	const TArray<FPlayLogEntry> Entries = FPlayLog::Get().Query(Filter, Limit, Skipped, OldestKept);
	TArray<TSharedPtr<FJsonValue>> Lines;
	for (const FPlayLogEntry& Entry : Entries)
	{
		Lines.Add(MakeShared<FJsonValueObject>(LogEntryJson(Entry, SessionTime)));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("since"), static_cast<double>(Filter.Since));
	Payload->SetNumberField(TEXT("last_seq"), static_cast<double>(FPlayLog::Get().GetLastSeq()));
	Payload->SetNumberField(TEXT("skipped"), Skipped);
	if (Filter.Since + 1 < OldestKept)
	{
		Payload->SetBoolField(TEXT("truncated"), true);
	}
	Payload->SetArrayField(TEXT("entries"), Lines);
	return FLevelToolResult::Success(FString::Printf(TEXT("%d log lines%s"), Lines.Num(),
		Skipped > 0 ? *FString::Printf(TEXT(" (%d older matches left out; raise limit or narrow the filter)"), Skipped) : TEXT("")), Payload);
}
} // namespace

FAssetDocumentDiagnostic PlayInvalidRequest(const FString& Path, const FString& Message)
{
	return MakePlayDiagnostic(Path, TEXT("Play.InvalidRequest"), Message);
}

UActorComponent* FindPlayComponent(AActor& Actor, const FString& Name)
{
	TInlineComponentArray<UActorComponent*> Components(&Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr && Component->GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			return Component;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonValue> EncodePlayValue(FProperty& Property, const void* ValuePtr)
{
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
	{
		const UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
		if (Object == nullptr)
		{
			return MakeShared<FJsonValueNull>();
		}
		return MakeShared<FJsonValueString>(DescribeObject(Object));
	}
	FAssetDocumentRuntimeValueDto Value;
	FString Code;
	if (UE::AssetDocument::EditorPropertyValueCodec::Encode(Property, ValuePtr, Value, Code))
	{
		if (TSharedPtr<FJsonValue> Json = ProjectAssetDocumentRuntimeValue(Value))
		{
			return Json;
		}
	}
	// Types the document codec does not represent (delegates, some containers): the Details text.
	FString Text;
	Property.ExportTextItem_Direct(Text, ValuePtr, nullptr, nullptr, PPF_None);
	return MakeShared<FJsonValueString>(Text);
}

FPlayWorldScope::FPlayWorldScope(UWorld& World)
{
	if (!GIsPlayInEditorWorld && GEditor != nullptr && &World == GEditor->PlayWorld)
	{
		Previous = SetPlayInEditorWorld(&World);
		bSwitched = true;
	}
}

FPlayWorldScope::~FPlayWorldScope()
{
	if (bSwitched)
	{
		RestoreEditorWorld(Previous);
	}
}

TArray<TSharedPtr<FJsonValue>> PlayStringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

FAssetDocumentDiagnostic MakePlayDiagnostic(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details)
{
	return MakeLevelDiagnostic(Path, Code, Message, MoveTemp(Details));
}

TSharedRef<FJsonObject> PlayVectorJson(const FVector& Vector)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("X"), Round2(Vector.X));
	Json->SetNumberField(TEXT("Y"), Round2(Vector.Y));
	Json->SetNumberField(TEXT("Z"), Round2(Vector.Z));
	return Json;
}

UWorld* GetPlayWorld()
{
	return GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
}

APlayerController* GetLocalPlayerController(UWorld& World)
{
	for (FConstPlayerControllerIterator It = World.GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* Controller = It->Get();
		if (Controller != nullptr && Controller->IsLocalController())
		{
			return Controller;
		}
	}
	return nullptr;
}

FPlayActorResolution ResolvePlayActor(UWorld& World, const TSharedPtr<FJsonValue>& Ref)
{
	FPlayActorResolution Result;
	FString Text;
	FString Kind = TEXT("any");
	if (Ref.IsValid() && Ref->Type == EJson::String)
	{
		Text = Ref->AsString();
	}
	else if (Ref.IsValid() && Ref->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Ref->AsObject();
		for (const TCHAR* Field : {TEXT("path"), TEXT("label"), TEXT("name")})
		{
			if (Object->TryGetStringField(Field, Text))
			{
				Kind = Field;
				break;
			}
		}
	}
	if (Text.IsEmpty())
	{
		Result.Code = TEXT("Play.InvalidRequest");
		Result.Message = TEXT("An actor reference is \"player\", \"controller\", \"game_mode\", \"game_state\", \"player_state\", \"hud\", "
			"\"camera\", an actor name, label or path, or {\"name\" | \"label\" | \"path\"}.");
		return Result;
	}

	if (Kind == TEXT("any"))
	{
		APlayerController* Controller = GetLocalPlayerController(World);
		const FString Lower = Text.ToLower();
		AActor* Special = nullptr;
		bool bSpecial = true;
		if (Lower == TEXT("player") || Lower == TEXT("pawn"))
		{
			Special = Controller != nullptr ? Controller->GetPawn() : nullptr;
		}
		else if (Lower == TEXT("controller"))
		{
			Special = Controller;
		}
		else if (Lower == TEXT("game_mode"))
		{
			Special = World.GetAuthGameMode();
		}
		else if (Lower == TEXT("game_state"))
		{
			Special = World.GetGameState();
		}
		else if (Lower == TEXT("player_state"))
		{
			Special = Controller != nullptr ? Controller->PlayerState.Get() : nullptr;
		}
		else if (Lower == TEXT("hud"))
		{
			Special = Controller != nullptr ? Controller->GetHUD() : nullptr;
		}
		else if (Lower == TEXT("camera"))
		{
			Special = Controller != nullptr ? Controller->PlayerCameraManager.Get() : nullptr;
		}
		else
		{
			bSpecial = false;
		}
		if (bSpecial)
		{
			if (Special == nullptr)
			{
				Result.Code = TEXT("Play.ActorNotFound");
				Result.Message = FString::Printf(TEXT("The play session has no %s right now%s."), *Text,
					Controller == nullptr ? TEXT(" (no local player: Simulate, or the player has not joined yet)") : TEXT(""));
			}
			Result.Actor = Special;
			return Result;
		}
	}

	if (Kind == TEXT("any") || Kind == TEXT("path"))
	{
		if (Text.Contains(TEXT(".")) || Text.StartsWith(TEXT("/")))
		{
			FSoftObjectPath SoftPath(Text);
			const int32 PieInstance = World.GetOutermost()->GetPIEInstanceID();
			if (PieInstance != INDEX_NONE)
			{
				SoftPath.FixupForPIE(PieInstance);
			}
			AActor* Found = Cast<AActor>(SoftPath.ResolveObject());
			if (Found == nullptr)
			{
				Found = FindObject<AActor>(nullptr, *Text);
			}
			if (Found != nullptr && Found->GetWorld() == &World && IsListed(Found))
			{
				Result.Actor = Found;
				return Result;
			}
			if (Kind == TEXT("path"))
			{
				Result.Code = TEXT("Play.ActorNotFound");
				Result.Message = FString::Printf(TEXT("No actor at '%s' in the play world (it may have been destroyed)."), *Text);
				return Result;
			}
		}
	}

	TArray<AActor*> ByLabel;
	TArray<AActor*> ByName;
	TArray<FString> Close;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsListed(Actor))
		{
			continue;
		}
		const FString Label = Actor->GetActorLabel(false);
		if ((Kind == TEXT("any") || Kind == TEXT("label")) && !Label.IsEmpty() && Label.Equals(Text, ESearchCase::IgnoreCase))
		{
			ByLabel.Add(Actor);
		}
		if ((Kind == TEXT("any") || Kind == TEXT("name")) && Actor->GetName().Equals(Text, ESearchCase::IgnoreCase))
		{
			ByName.Add(Actor);
		}
		if (Close.Num() < 10 && (Actor->GetName().Contains(Text, ESearchCase::IgnoreCase) || (!Label.IsEmpty() && Label.Contains(Text, ESearchCase::IgnoreCase))))
		{
			Close.Add(Actor->GetName());
		}
	}
	if (ByName.Num() == 1)
	{
		Result.Actor = ByName[0];
		return Result;
	}
	if (ByLabel.Num() == 1)
	{
		Result.Actor = ByLabel[0];
		return Result;
	}
	if (ByLabel.Num() > 1)
	{
		Result.Code = TEXT("Play.ActorRefAmbiguous");
		for (const AActor* Actor : ByLabel)
		{
			Result.Candidates.Add(Actor->GetName());
		}
		Result.Message = FString::Printf(TEXT("%d actors are labelled '%s': %s. Use the name."), ByLabel.Num(), *Text,
			*FString::Join(Result.Candidates, TEXT(", ")));
		return Result;
	}
	Result.Code = TEXT("Play.ActorNotFound");
	Result.Candidates = Close;
	Result.Message = FString::Printf(TEXT("No actor named or labelled '%s' in the play world.%s"), *Text,
		Close.IsEmpty() ? TEXT(" query_play lists the actors.") : *FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", "))));
	return Result;
}

bool ReadActorValue(AActor& Actor, const FString& Path, TSharedPtr<FJsonValue>& OutValue, FString& OutError)
{
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."));
	if (Segments.IsEmpty())
	{
		OutError = TEXT("empty property path");
		return false;
	}
	const UStruct* Struct = Actor.GetClass();
	const void* Container = &Actor;
	int32 Index = 0;
	if (FindReadableProperty(*Struct, Segments[0]) == nullptr)
	{
		if (UActorComponent* Component = FindPlayComponent(Actor, Segments[0]))
		{
			if (Segments.Num() == 1)
			{
				OutValue = MakeShared<FJsonValueString>(DescribeObject(Component));
				return true;
			}
			Struct = Component->GetClass();
			Container = Component;
			Index = 1;
		}
	}
	for (; Index < Segments.Num(); ++Index)
	{
		FProperty* Property = FindReadableProperty(*Struct, Segments[Index]);
		if (Property == nullptr)
		{
			const TArray<FString> Close = CloseNames(*Struct, Segments[Index]);
			OutError = FString::Printf(TEXT("'%s' has no property or component '%s'%s"), *Struct->GetName(), *Segments[Index],
				Close.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (close: %s)"), *FString::Join(Close, TEXT(", "))));
			return false;
		}
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
		if (Index == Segments.Num() - 1)
		{
			OutValue = EncodeValue(*Property, ValuePtr);
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
				OutValue = MakeShared<FJsonValueNull>();
				return true;
			}
			Struct = Next->GetClass();
			Container = Next;
			continue;
		}
		OutError = FString::Printf(TEXT("'%s' is not a struct or object; '%s' cannot be read inside it"), *Segments[Index], *Segments[Index + 1]);
		return false;
	}
	return false;
}

TSharedRef<FJsonObject> PlayActorJson(AActor& Actor, const FPlayActorJsonOptions& Options)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Actor.GetName());
	const FString Label = Actor.GetActorLabel(false);
	if (!Label.IsEmpty() && Label != Actor.GetName())
	{
		Json->SetStringField(TEXT("label"), Label);
	}
	Json->SetStringField(TEXT("class"), Actor.GetClass()->GetPathName());
	// The full play-world path ("/Game/Maps/UEDPIE_0_Arena.Arena:PersistentLevel.BP_Boss_C_0"): unique where the
	// object name is not (the editor's copy of a placed actor has the same name).
	Json->SetStringField(TEXT("path"), Actor.GetPathName());
	if (Actor.GetRootComponent() != nullptr)
	{
		Json->SetObjectField(TEXT("location"), PlayVectorJson(Actor.GetActorLocation()));
		Json->SetObjectField(TEXT("rotation"), RotatorJson(Actor.GetActorRotation()));
		const FVector Velocity = Actor.GetVelocity();
		if (Velocity.Size() > 1.0)
		{
			Json->SetObjectField(TEXT("velocity"), PlayVectorJson(Velocity));
			Json->SetNumberField(TEXT("speed"), Round2(Velocity.Size()));
		}
	}
	if (Options.DistanceFrom.IsSet())
	{
		Json->SetNumberField(TEXT("distance"), Round2(FVector::Dist(Actor.GetActorLocation(), Options.DistanceFrom.GetValue())));
	}
	if (Actor.IsHidden())
	{
		Json->SetBoolField(TEXT("hidden"), true);
	}
	if (!Actor.Tags.IsEmpty())
	{
		TArray<FString> Tags;
		for (const FName& Tag : Actor.Tags)
		{
			Tags.Add(Tag.ToString());
		}
		Json->SetArrayField(TEXT("tags"), PlayStringArray(Tags));
	}
	if (const APawn* Pawn = Cast<APawn>(&Actor))
	{
		if (const AController* Controller = Pawn->GetController())
		{
			TSharedRef<FJsonObject> ControllerJson = MakeShared<FJsonObject>();
			ControllerJson->SetStringField(TEXT("name"), Controller->GetName());
			ControllerJson->SetStringField(TEXT("class"), Controller->GetClass()->GetPathName());
			ControllerJson->SetBoolField(TEXT("player"), Controller->IsPlayerController());
			Json->SetObjectField(TEXT("controller"), ControllerJson);
		}
	}
	else if (const AController* AsController = Cast<AController>(&Actor))
	{
		if (const APawn* Possessed = AsController->GetPawn())
		{
			Json->SetStringField(TEXT("pawn"), Possessed->GetName());
		}
	}

	TSharedRef<FJsonObject> Vitals = MakeShared<FJsonObject>();
	AddVitals(Actor, FString(), Vitals);
	TInlineComponentArray<UActorComponent*> Components(&Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr && Component->GetClass()->ClassGeneratedBy != nullptr)
		{
			AddVitals(*Component, Component->GetName() + TEXT("."), Vitals);
		}
	}
	if (Vitals->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("vitals"), Vitals);
	}
	if (TSharedPtr<FJsonObject> Montage = MontageJson(Actor))
	{
		Json->SetObjectField(TEXT("montage"), Montage);
	}
	AAIController* AI = Cast<AAIController>(&Actor);
	if (AI == nullptr)
	{
		if (const APawn* Pawn = Cast<APawn>(&Actor))
		{
			AI = Cast<AAIController>(Pawn->GetController());
		}
	}
	if (AI != nullptr)
	{
		Json->SetObjectField(TEXT("ai"), AiJson(*AI));
	}

	if (!Options.Properties.IsEmpty())
	{
		TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Errors = MakeShared<FJsonObject>();
		for (const FString& Path : Options.Properties)
		{
			TSharedPtr<FJsonValue> Value;
			FString Error;
			if (ReadActorValue(Actor, Path, Value, Error))
			{
				Values->SetField(Path, Value);
			}
			else
			{
				Errors->SetStringField(Path, Error);
			}
		}
		Json->SetObjectField(TEXT("values"), Values);
		if (Errors->Values.Num() > 0)
		{
			Json->SetObjectField(TEXT("value_errors"), Errors);
		}
	}

	if (Options.bComponents || Options.bProperties)
	{
		TArray<TSharedPtr<FJsonValue>> ComponentList;
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		if (Options.bProperties)
		{
			AddChangedProperties(Actor, FString(), Options.bDiffOnly, Properties);
		}
		for (UActorComponent* Component : Components)
		{
			if (Component == nullptr || Component->IsVisualizationComponent())
			{
				continue;
			}
			TSharedRef<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
			ComponentJson->SetStringField(TEXT("name"), Component->GetName());
			ComponentJson->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
			if (!Component->IsActive())
			{
				ComponentJson->SetBoolField(TEXT("active"), false);
			}
			ComponentList.Add(MakeShared<FJsonValueObject>(ComponentJson));
			if (Options.bProperties)
			{
				AddChangedProperties(*Component, Component->GetName() + TEXT("."), Options.bDiffOnly, Properties);
			}
		}
		Json->SetArrayField(TEXT("components"), ComponentList);
		if (Options.bProperties)
		{
			Json->SetObjectField(TEXT("properties"), Properties);
		}
	}
	return Json;
}

bool ParseCondition(UWorld* World, const TSharedPtr<FJsonValue>& Json, const FString& Path, FPlayCondition& Out,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	const TSharedPtr<FJsonObject> Object = Json.IsValid() && Json->Type == EJson::Object ? Json->AsObject() : nullptr;
	const FString Shapes = TEXT("A condition is {\"actor\", \"property\", \"op\"?, \"value\"}, {\"actor\", \"exists\": false}, "
		"{\"count\": {class?, name?, tag?}, \"op\"?, \"value\"}, {\"log\": \"text\", \"category\"?} or {\"session\": \"ended\"}.");
	if (!Object.IsValid())
	{
		OutDiagnostics.Add(PlayInvalidRequest(Path, Shapes));
		return false;
	}
	Object->TryGetStringField(TEXT("op"), Out.Op);
	static const TCHAR* const Ops[] = {TEXT("=="), TEXT("!="), TEXT("<"), TEXT("<="), TEXT(">"), TEXT(">=")};
	bool bKnownOp = false;
	for (const TCHAR* Op : Ops)
	{
		bKnownOp |= Out.Op == Op;
	}
	if (!bKnownOp)
	{
		OutDiagnostics.Add(PlayInvalidRequest(Path + TEXT("/op"), TEXT("'op' is ==, !=, <, <=, > or >=.")));
		return false;
	}
	Out.Value = Object->TryGetField(TEXT("value"));
	FString Session;
	if (Object->TryGetStringField(TEXT("session"), Session))
	{
		if (Session != TEXT("ended"))
		{
			OutDiagnostics.Add(PlayInvalidRequest(Path + TEXT("/session"), TEXT("'session' is \"ended\".")));
			return false;
		}
		Out.Kind = FPlayCondition::EKind::SessionEnded;
		Out.Text = TEXT("the play session ended");
		return true;
	}
	if (Object->TryGetStringField(TEXT("log"), Out.LogText))
	{
		Out.Kind = FPlayCondition::EKind::Log;
		Object->TryGetStringField(TEXT("category"), Out.LogCategory);
		Out.Text = FString::Printf(TEXT("a log line contains '%s'"), *Out.LogText);
		return true;
	}
	const TSharedPtr<FJsonObject>* Count = nullptr;
	if (Object->TryGetObjectField(TEXT("count"), Count) && Count != nullptr)
	{
		Out.Kind = FPlayCondition::EKind::Count;
		Out.Filter = *Count;
		if (World != nullptr)
		{
			FActorFilter Filter;
			if (!ParseActorFilter(*World, *Out.Filter, Path + TEXT("/count"), Filter, OutDiagnostics))
			{
				return false;
			}
		}
		if (!Out.Value.IsValid() || Out.Value->Type != EJson::Number)
		{
			OutDiagnostics.Add(PlayInvalidRequest(Path + TEXT("/value"), TEXT("A count condition needs a number 'value'.")));
			return false;
		}
		Out.Text = FString::Printf(TEXT("actor count %s %g"), *Out.Op, Out.Value->AsNumber());
		return true;
	}
	if (!Object->HasField(TEXT("actor")))
	{
		OutDiagnostics.Add(PlayInvalidRequest(Path, Shapes));
		return false;
	}
	Out.ActorRef = Object->TryGetField(TEXT("actor"));
	FPlayActorResolution Resolution;
	if (World != nullptr)
	{
		Resolution = ResolvePlayActor(*World, Out.ActorRef);
	}
	if (Object->TryGetBoolField(TEXT("exists"), Out.bExists))
	{
		Out.Kind = FPlayCondition::EKind::Exists;
		Out.ExistsActor = Resolution.Actor;
		if (!Out.bExists && Resolution.Actor == nullptr)
		{
			OutDiagnostics.Add(MakePlayDiagnostic(Path + TEXT("/actor"), Resolution.Code, Resolution.Message));
			return false;
		}
		Out.Text = FString::Printf(TEXT("%s %s"), *RefText(Out.ActorRef), Out.bExists ? TEXT("exists") : TEXT("is destroyed"));
		return true;
	}
	if (!Object->TryGetStringField(TEXT("property"), Out.Property) || Out.Property.IsEmpty())
	{
		OutDiagnostics.Add(PlayInvalidRequest(Path + TEXT("/property"), TEXT("An actor condition needs 'property' (or \"exists\": false).")));
		return false;
	}
	if (!Out.Value.IsValid())
	{
		OutDiagnostics.Add(PlayInvalidRequest(Path + TEXT("/value"), TEXT("An actor condition needs 'value'.")));
		return false;
	}
	if (World != nullptr)
	{
		if (Resolution.Actor == nullptr)
		{
			OutDiagnostics.Add(MakePlayDiagnostic(Path + TEXT("/actor"), Resolution.Code, Resolution.Message));
			return false;
		}
		TSharedPtr<FJsonValue> Current;
		FString Error;
		if (!ReadActorValue(*Resolution.Actor, Out.Property, Current, Error))
		{
			OutDiagnostics.Add(MakePlayDiagnostic(Path + TEXT("/property"), TEXT("Play.PropertyNotFound"), Error));
			return false;
		}
	}
	FString ValueText;
	FJsonSerializer::Serialize(Out.Value, FString(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ValueText));
	Out.Text = FString::Printf(TEXT("%s.%s %s %s"), *RefText(Out.ActorRef),
		*Out.Property, *Out.Op, *ValueText);
	return true;
}

bool EvaluateCondition(UWorld* World, const FPlayCondition& Condition, uint64 LogSince, TSharedPtr<FJsonValue>& OutObserved)
{
	switch (Condition.Kind)
	{
	case FPlayCondition::EKind::SessionEnded:
		return World == nullptr;
	case FPlayCondition::EKind::Log:
	{
		FlushPendingLog();
		FPlayLogFilter Filter;
		Filter.Since = LogSince;
		Filter.Text = Condition.LogText;
		Filter.MaxVerbosity = ELogVerbosity::VeryVerbose;
		if (!Condition.LogCategory.IsEmpty())
		{
			Filter.Categories.Add(Condition.LogCategory);
		}
		int32 Skipped = 0;
		uint64 Oldest = 0;
		const TArray<FPlayLogEntry> Found = FPlayLog::Get().Query(Filter, 1, Skipped, Oldest);
		if (!Found.IsEmpty())
		{
			OutObserved = MakeShared<FJsonValueString>(Found[0].Message.Left(300));
			return true;
		}
		return false;
	}
	case FPlayCondition::EKind::Exists:
	{
		const AActor* Actor = Condition.ExistsActor.Get();
		const bool bExists = IsListed(Actor);
		if (Condition.bExists && World != nullptr && !bExists)
		{
			const FPlayActorResolution Resolution = ResolvePlayActor(*World, Condition.ActorRef);
			OutObserved = MakeShared<FJsonValueBoolean>(Resolution.Actor != nullptr);
			return Resolution.Actor != nullptr;
		}
		OutObserved = MakeShared<FJsonValueBoolean>(bExists);
		return bExists == Condition.bExists;
	}
	case FPlayCondition::EKind::Count:
	{
		if (World == nullptr)
		{
			return false;
		}
		FActorFilter Filter;
		TArray<FAssetDocumentDiagnostic> Ignored;
		ParseActorFilter(*World, *Condition.Filter, FString(), Filter, Ignored);
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			Count += IsListed(*It) && MatchesFilter(**It, Filter) ? 1 : 0;
		}
		OutObserved = MakeShared<FJsonValueNumber>(Count);
		return CompareJson(OutObserved, Condition.Op, Condition.Value);
	}
	case FPlayCondition::EKind::Value:
	default:
	{
		if (World == nullptr)
		{
			return false;
		}
		const FPlayActorResolution Resolution = ResolvePlayActor(*World, Condition.ActorRef);
		if (Resolution.Actor == nullptr)
		{
			OutObserved = MakeShared<FJsonValueString>(TEXT("<actor not found>"));
			return false;
		}
		FString Error;
		if (!ReadActorValue(*Resolution.Actor, Condition.Property, OutObserved, Error))
		{
			OutObserved = MakeShared<FJsonValueString>(Error);
			return false;
		}
		return CompareJson(OutObserved, Condition.Op, Condition.Value);
	}
	}
}

bool ParsePlayBody(const FString& RequestBody, TSharedPtr<FJsonObject>& OutJson, FLevelToolResult& OutFailure)
{
	if (RequestBody.TrimStartAndEnd().IsEmpty())
	{
		OutJson = MakeShared<FJsonObject>();
		return true;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, OutJson) || !OutJson.IsValid())
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(PlayInvalidRequest(TEXT(""), FString::Printf(TEXT("The request body is not a JSON object: %s"), *Reader->GetErrorMessage())));
		OutFailure = FLevelToolResult::Failure(MoveTemp(Diagnostics));
		return false;
	}
	return true;
}

FLevelToolResult RunPlayQuery(const TSharedPtr<FJsonObject>& Request)
{
	check(IsInGameThread());
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	if (!Request.IsValid())
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT(""), TEXT("The request must be a JSON object.")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	FString What = TEXT("actors");
	Request->TryGetStringField(TEXT("what"), What);
	if (What != TEXT("actors") && What != TEXT("log") && What != TEXT("effects") && What != TEXT("audio") && What != TEXT("input"))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/what"), TEXT("'what' is \"actors\" (default), \"log\", \"effects\" (Niagara / Cascade components), "
			"\"audio\" (playing audio components and the sounds started) or \"input\" (the player's mapping contexts and effective keys).")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	for (const auto& Field : Request->Values)
	{
		bool bKnown = false;
		for (const TCHAR* Known : QueryFields)
		{
			bKnown |= Field.Key == Known;
		}
		if (!bKnown || !IsQueryFieldOf(What, FString(*Field.Key)))
		{
			// The fields are flat: {"what": "log", "text": "..."}, never nested under the what ("log": {...}).
			Diagnostics.Add(PlayInvalidRequest(TEXT("/") + FString(*Field.Key), FString::Printf(TEXT("Unknown query_play field '%s'%s. With what=actors "
				"(default): %s. With what=log: %s. With what=effects: %s. With what=audio: %s. With what=input: none. The fields are flat (not nested under the "
				"what)."), *Field.Key, bKnown ? *FString::Printf(TEXT(" for what=%s"), *What) : TEXT(""), ActorQueryFields,
				*FString(LogQueryFields).Replace(TEXT(", "), TEXT(" / ")), EffectQueryFields, AudioQueryFields)));
		}
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (What == TEXT("log"))
	{
		return QueryLog(*Request);
	}

	UWorld* World = GetPlayWorld();
	if (World == nullptr)
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT(""), TEXT("Play.NoSession"),
			TEXT("No Play In Editor session is running: start one with play {\"action\": \"start\"}. (query_level reads the editor world.)")));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (What == TEXT("effects"))
	{
		return QueryEffects(*World, *Request);
	}
	if (What == TEXT("audio"))
	{
		return QueryAudio(*World, *Request);
	}
	if (What == TEXT("input"))
	{
		return QueryPlayInput(*World);
	}

	FActorFilter Filter;
	ParseActorFilter(*World, *Request, FString(), Filter, Diagnostics);
	FPlayActorJsonOptions Options;
	FString Detail = TEXT("summary");
	Request->TryGetStringField(TEXT("detail"), Detail);
	if (Detail != TEXT("summary") && Detail != TEXT("components") && Detail != TEXT("properties") && Detail != TEXT("full"))
	{
		Diagnostics.Add(PlayInvalidRequest(TEXT("/detail"), TEXT("'detail' is \"summary\" (default), \"components\" (adds the components) or "
			"\"properties\" (adds the components and the changed properties; \"full\" is the same), as in query_level.")));
	}
	Options.bComponents = Detail != TEXT("summary");
	Options.bProperties = Detail == TEXT("properties") || Detail == TEXT("full");
	Request->TryGetBoolField(TEXT("diff_only"), Options.bDiffOnly);
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	if (Request->TryGetArrayField(TEXT("properties"), Properties) && Properties != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *Properties)
		{
			FString Path;
			if (Entry.IsValid() && Entry->TryGetString(Path) && !Path.IsEmpty())
			{
				Options.Properties.Add(Path);
			}
			else
			{
				Diagnostics.Add(PlayInvalidRequest(TEXT("/properties"), TEXT("'properties' is an array of property paths (\"Health\", \"BPC_Combat.Health\").")));
				break;
			}
		}
	}
	int32 Limit = DefaultLimit;
	int32 Offset = 0;
	double Number = 0.0;
	if (Request->TryGetNumberField(TEXT("limit"), Number))
	{
		Limit = FMath::Clamp(static_cast<int32>(Number), 1, MaximumLimit);
	}
	if (Request->TryGetNumberField(TEXT("offset"), Number))
	{
		Offset = FMath::Max(0, static_cast<int32>(Number));
	}

	TArray<AActor*> Candidates;
	const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
	if (Request->TryGetArrayField(TEXT("actors"), Refs) && Refs != nullptr)
	{
		for (int32 Index = 0; Index < Refs->Num(); ++Index)
		{
			const FPlayActorResolution Resolution = ResolvePlayActor(*World, (*Refs)[Index]);
			if (Resolution.Actor == nullptr)
			{
				TSharedPtr<FJsonObject> Details;
				if (!Resolution.Candidates.IsEmpty())
				{
					Details = MakeShared<FJsonObject>();
					Details->SetArrayField(TEXT("candidates"), PlayStringArray(Resolution.Candidates));
				}
				Diagnostics.Add(MakePlayDiagnostic(FString::Printf(TEXT("/actors/%d"), Index), Resolution.Code, Resolution.Message, Details));
			}
			else if (MatchesFilter(*Resolution.Actor, Filter))
			{
				Candidates.AddUnique(Resolution.Actor);
			}
		}
	}
	else
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsListed(*It) && MatchesFilter(**It, Filter))
			{
				Candidates.Add(*It);
			}
		}
		if (Filter.NearLocation.IsSet())
		{
			const FVector Center = Filter.NearLocation.GetValue();
			Candidates.Sort([&Center](const AActor& Left, const AActor& Right)
			{
				return FVector::DistSquared(Left.GetActorLocation(), Center) < FVector::DistSquared(Right.GetActorLocation(), Center);
			});
		}
		else
		{
			Candidates.Sort([](const AActor& Left, const AActor& Right) { return Left.GetName() < Right.GetName(); });
		}
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	Options.DistanceFrom = Filter.NearLocation;

	TArray<TSharedPtr<FJsonValue>> Actors;
	const int32 First = FMath::Min(Offset, Candidates.Num());
	const int32 Last = FMath::Min(Candidates.Num(), First + Limit);
	for (int32 Index = First; Index < Last; ++Index)
	{
		Actors.Add(MakeShared<FJsonValueObject>(PlayActorJson(*Candidates[Index], Options)));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("world"), World->GetPathName());
	Payload->SetNumberField(TEXT("world_time"), Round2(World->GetTimeSeconds()));
	Payload->SetNumberField(TEXT("total"), Candidates.Num());
	Payload->SetNumberField(TEXT("offset"), Offset);
	Payload->SetNumberField(TEXT("limit"), Limit);
	Payload->SetArrayField(TEXT("actors"), Actors);
	return FLevelToolResult::Success(FString::Printf(TEXT("%d of %d actors at world time %.2f s"), Actors.Num(), Candidates.Num(),
		World->GetTimeSeconds()), Payload);
}

FLevelToolResult RunPlayQueryFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	FLevelToolResult Failure;
	return ParsePlayBody(RequestBody, Json, Failure) ? RunPlayQuery(Json) : Failure;
}
}
