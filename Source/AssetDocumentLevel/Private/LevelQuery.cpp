// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "LevelQuery.h"

#include "Algo/Sort.h"
#include "AssetDocumentEditorObjectProperties.h"
#include "AssetDocumentLevelActorRef.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorActorFolders.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "Misc/EngineVersionComparison.h"
#include "GameFramework/Actor.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Layers/LayersSubsystem.h"
#include "LevelUtils.h"
#include "LevelWorldIndex.h"
#include "Misc/PackageName.h"
#include "Play/PlayQuery.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/WorldPartition.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
namespace ObjectProperties = UE::AssetDocument::EditorObjectProperties;

constexpr int32 DefaultLimit = 100;
constexpr int32 MaximumLimit = 1000;

enum class EQueryDetail : uint8
{
	Summary,
	Components,
	Properties,
};

struct FQueryRequest
{
	bool bWorldSummary = false;
	bool bHasActors = false;
	TArray<FActorRef> Actors;
	UClass* Class = nullptr;
	bool bExactClass = false;
	FString Name;
	FString Folder;
	FString Tag;
	FString Layer;
	FString DataLayer;
	FString Level;
	TOptional<FVector> NearLocation;
	double NearRadius = 0.0;
	TOptional<FBox> Bounds;
	bool bSelectedOnly = false;
	bool bIncludeUnloaded = false;
	EQueryDetail Detail = EQueryDetail::Summary;
	bool bDiffOnly = true;
	// properties: value paths read on each actor ("RootComponent.RelativeLocation", "PointLightComponent.Intensity"), as query_play reads them.
	TArray<FString> Properties;
	int32 Limit = DefaultLimit;
	int32 Offset = 0;
};

const TCHAR* const KnownFields[] = {
	TEXT("what"), TEXT("actors"), TEXT("class"), TEXT("exact_class"), TEXT("name"), TEXT("folder"), TEXT("tag"),
	TEXT("layer"), TEXT("data_layer"), TEXT("level"), TEXT("near"), TEXT("bounds"), TEXT("selected_only"),
	TEXT("include_unloaded"), TEXT("detail"), TEXT("diff_only"), TEXT("properties"), TEXT("limit"), TEXT("offset")};

FString Pointer(const FString& Field)
{
	return TEXT("/") + Field;
}

FAssetDocumentDiagnostic InvalidRequest(const FString& Path, const FString& Message)
{
	return MakeLevelDiagnostic(Path, TEXT("Level.InvalidRequest"), Message);
}

bool ReadString(const FJsonObject& Json, const TCHAR* Field, FString& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!Json.HasField(Field))
	{
		return true;
	}
	if (!Json.TryGetStringField(Field, Out) || Out.IsEmpty())
	{
		Diagnostics.Add(InvalidRequest(Pointer(Field), FString::Printf(TEXT("'%s' must be a non-empty string."), Field)));
		return false;
	}
	return true;
}

bool ReadBool(const FJsonObject& Json, const TCHAR* Field, bool& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (Json.HasField(Field) && !Json.TryGetBoolField(Field, Out))
	{
		Diagnostics.Add(InvalidRequest(Pointer(Field), FString::Printf(TEXT("'%s' must be true or false."), Field)));
		return false;
	}
	return true;
}

bool ReadInt(const FJsonObject& Json, const TCHAR* Field, int32 Min, int32 Max, int32& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!Json.HasField(Field))
	{
		return true;
	}
	double Number = 0.0;
	if (!Json.TryGetNumberField(Field, Number) || Number != FMath::FloorToDouble(Number) || Number < Min || Number > Max)
	{
		Diagnostics.Add(InvalidRequest(Pointer(Field), FString::Printf(TEXT("'%s' must be an integer from %d to %d."), Field, Min, Max)));
		return false;
	}
	Out = static_cast<int32>(Number);
	return true;
}

// {"X", "Y", "Z"} (members default to 0), as AssetDocument writes vectors.
bool ReadVector(const TSharedPtr<FJsonValue>& Value, const FString& Path, FVector& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Value.IsValid() || !Value->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
	{
		Diagnostics.Add(InvalidRequest(Path, TEXT("expected a vector {\"X\": .., \"Y\": .., \"Z\": ..}.")));
		return false;
	}
	Out = FVector::ZeroVector;
	for (const auto& Member : (*Object)->Values)
	{
		double Number = 0.0;
		if (!Member.Value.IsValid() || !Member.Value->TryGetNumber(Number)
			|| (Member.Key != TEXT("X") && Member.Key != TEXT("Y") && Member.Key != TEXT("Z")))
		{
			Diagnostics.Add(InvalidRequest(Path + TEXT("/") + Member.Key, TEXT("a vector has the number members X, Y and Z.")));
			return false;
		}
		(Member.Key == TEXT("X") ? Out.X : Member.Key == TEXT("Y") ? Out.Y : Out.Z) = Number;
	}
	return true;
}

UClass* ResolveClass(const FString& Path)
{
	UClass* Class = FindObject<UClass>(nullptr, *Path);
	if (Class == nullptr && FPackageName::IsValidObjectPath(Path))
	{
		Class = LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (Class == nullptr && !Path.EndsWith(TEXT("_C")) && FPackageName::IsValidObjectPath(Path))
	{
		// A Blueprint asset path names its generated class.
		Class = LoadObject<UClass>(nullptr, *(Path + TEXT("_C")), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return Class != nullptr && Class->IsChildOf<AActor>() ? Class : nullptr;
}

bool ParseRequest(const FJsonObject& Json, FQueryRequest& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	for (const auto& Field : Json.Values)
	{
		bool bKnown = false;
		for (const TCHAR* Known : KnownFields)
		{
			bKnown |= Field.Key == Known;
		}
		if (!bKnown)
		{
			Diagnostics.Add(InvalidRequest(Pointer(FString(*Field.Key)), FString::Printf(
				TEXT("Unknown query_level field '%s'. Fields: what, actors, class, exact_class, name, folder, tag, layer, "
					"data_layer, level, near, bounds, selected_only, include_unloaded, detail, diff_only, properties, limit, offset."),
				*Field.Key)));
		}
	}

	FString What = TEXT("actors");
	if (ReadString(Json, TEXT("what"), What, Diagnostics) && What != TEXT("actors") && What != TEXT("world"))
	{
		Diagnostics.Add(InvalidRequest(Pointer(TEXT("what")), TEXT("'what' is \"actors\" (default) or \"world\".")));
	}
	Out.bWorldSummary = What == TEXT("world");

	if (Json.HasField(TEXT("actors")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
		if (!Json.TryGetArrayField(TEXT("actors"), Refs) || Refs == nullptr)
		{
			Diagnostics.Add(InvalidRequest(Pointer(TEXT("actors")), TEXT("'actors' must be an array of actor references.")));
		}
		else
		{
			Out.bHasActors = true;
			for (int32 Index = 0; Index < Refs->Num(); ++Index)
			{
				FActorRef Ref;
				FString Error;
				if (!ParseActorRef((*Refs)[Index], Ref, Error))
				{
					Diagnostics.Add(InvalidRequest(FString::Printf(TEXT("/actors/%d"), Index), Error));
				}
				else if (Ref.Kind == EActorRefKind::BatchId)
				{
					Diagnostics.Add(InvalidRequest(FString::Printf(TEXT("/actors/%d"), Index),
						TEXT("batch ids (\"@id\") exist only inside one edit_level request; use the actor's path.")));
				}
				else
				{
					Out.Actors.Add(MoveTemp(Ref));
				}
			}
		}
	}

	FString ClassPath;
	if (ReadString(Json, TEXT("class"), ClassPath, Diagnostics) && !ClassPath.IsEmpty())
	{
		Out.Class = ResolveClass(ClassPath);
		if (Out.Class == nullptr)
		{
			Diagnostics.Add(InvalidRequest(Pointer(TEXT("class")), FString::Printf(
				TEXT("'%s' is not an actor class path (e.g. /Script/Engine.PointLight, /Game/Blueprints/BP_Door.BP_Door_C)."),
				*ClassPath)));
		}
	}
	ReadBool(Json, TEXT("exact_class"), Out.bExactClass, Diagnostics);
	if (Out.bExactClass && !Json.HasField(TEXT("class")))
	{
		Diagnostics.Add(InvalidRequest(Pointer(TEXT("exact_class")), TEXT("'exact_class' needs 'class'.")));
	}
	ReadString(Json, TEXT("name"), Out.Name, Diagnostics);
	if (ReadString(Json, TEXT("folder"), Out.Folder, Diagnostics))
	{
		Out.Folder.RemoveFromEnd(TEXT("/"));
	}
	ReadString(Json, TEXT("tag"), Out.Tag, Diagnostics);
	ReadString(Json, TEXT("layer"), Out.Layer, Diagnostics);
	ReadString(Json, TEXT("data_layer"), Out.DataLayer, Diagnostics);
	ReadString(Json, TEXT("level"), Out.Level, Diagnostics);

	if (Json.HasField(TEXT("near")))
	{
		const TSharedPtr<FJsonObject>* Near = nullptr;
		double Radius = 0.0;
		FVector Location;
		if (!Json.TryGetObjectField(TEXT("near"), Near) || Near == nullptr || !(*Near)->HasField(TEXT("location"))
			|| !(*Near)->TryGetNumberField(TEXT("radius"), Radius) || Radius < 0.0 || (*Near)->Values.Num() != 2)
		{
			Diagnostics.Add(InvalidRequest(Pointer(TEXT("near")),
				TEXT("'near' is {\"location\": {\"X\", \"Y\", \"Z\"}, \"radius\": <number >= 0>}.")));
		}
		else if (ReadVector((*Near)->TryGetField(TEXT("location")), TEXT("/near/location"), Location, Diagnostics))
		{
			Out.NearLocation = Location;
			Out.NearRadius = Radius;
		}
	}
	if (Json.HasField(TEXT("bounds")))
	{
		const TSharedPtr<FJsonObject>* Bounds = nullptr;
		FVector Min;
		FVector Max;
		if (!Json.TryGetObjectField(TEXT("bounds"), Bounds) || Bounds == nullptr || (*Bounds)->Values.Num() != 2
			|| !(*Bounds)->HasField(TEXT("min")) || !(*Bounds)->HasField(TEXT("max")))
		{
			Diagnostics.Add(InvalidRequest(Pointer(TEXT("bounds")), TEXT("'bounds' is {\"min\": {\"X\", \"Y\", \"Z\"}, \"max\": {...}}.")));
		}
		else if (ReadVector((*Bounds)->TryGetField(TEXT("min")), TEXT("/bounds/min"), Min, Diagnostics)
			&& ReadVector((*Bounds)->TryGetField(TEXT("max")), TEXT("/bounds/max"), Max, Diagnostics))
		{
			Out.Bounds = FBox(Min, Max);
		}
	}
	ReadBool(Json, TEXT("selected_only"), Out.bSelectedOnly, Diagnostics);
	ReadBool(Json, TEXT("include_unloaded"), Out.bIncludeUnloaded, Diagnostics);

	FString Detail = TEXT("summary");
	if (ReadString(Json, TEXT("detail"), Detail, Diagnostics))
	{
		if (Detail == TEXT("summary"))
		{
			Out.Detail = EQueryDetail::Summary;
		}
		else if (Detail == TEXT("components"))
		{
			Out.Detail = EQueryDetail::Components;
		}
		else if (Detail == TEXT("properties") || Detail == TEXT("full"))
		{
			Out.Detail = EQueryDetail::Properties;
		}
		else
		{
			Diagnostics.Add(InvalidRequest(Pointer(TEXT("detail")), TEXT("'detail' is \"summary\" (default), \"components\" or \"properties\" "
				"(\"full\" is the same), as in query_play.")));
		}
	}
	ReadBool(Json, TEXT("diff_only"), Out.bDiffOnly, Diagnostics);
	if (Json.HasField(TEXT("properties")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
		bool bValid = Json.TryGetArrayField(TEXT("properties"), Paths) && Paths != nullptr;
		for (int32 Index = 0; bValid && Index < Paths->Num(); ++Index)
		{
			FString Path;
			bValid = (*Paths)[Index].IsValid() && (*Paths)[Index]->Type == EJson::String && (*Paths)[Index]->TryGetString(Path) && !Path.IsEmpty();
			Out.Properties.Add(Path);
		}
		if (!bValid)
		{
			Diagnostics.Add(InvalidRequest(Pointer(TEXT("properties")), TEXT("'properties' is an array of value paths (\"bHidden\", "
				"\"PointLightComponent.Intensity\", \"RootComponent.RelativeLocation.Z\").")));
		}
	}
	ReadInt(Json, TEXT("limit"), 1, MaximumLimit, Out.Limit, Diagnostics);
	ReadInt(Json, TEXT("offset"), 0, MAX_int32, Out.Offset, Diagnostics);
	return Diagnostics.IsEmpty();
}

TSharedPtr<FJsonValue> NullValue()
{
	return MakeShared<FJsonValueNull>();
}

TSharedPtr<FJsonValue> StringOrNull(const FString& Value)
{
	return Value.IsEmpty() ? NullValue() : MakeShared<FJsonValueString>(Value);
}

TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

TArray<FString> NamesToStrings(const TArray<FName>& Names)
{
	TArray<FString> Result;
	for (const FName& Name : Names)
	{
		Result.Add(Name.ToString());
	}
	return Result;
}

// Numbers through the AssetDocument projection (shortest round-trip decimals).
TSharedRef<FJsonObject> NumbersJson(std::initializer_list<TPair<const TCHAR*, double>> Members)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	for (const TPair<const TCHAR*, double>& Member : Members)
	{
		FAssetDocumentRuntimeValueDto Number;
		Number.Kind = EAssetDocumentRuntimeValueKind::Number;
		Number.NumberValue = Member.Value;
		Value.ObjectKeys.Add(Member.Key);
		Value.Elements.Add(Number);
	}
	return ProjectAssetDocumentRuntimeValue(Value)->AsObject().ToSharedRef();
}

TSharedRef<FJsonObject> VectorJson(const FVector& Vector)
{
	return NumbersJson({{TEXT("X"), Vector.X}, {TEXT("Y"), Vector.Y}, {TEXT("Z"), Vector.Z}});
}

// The reflected fields a query lists, cached per class for one request.
class FFieldCache
{
public:
	const TArray<ObjectProperties::FAdmittedField>& Get(UClass& Class, bool bActor)
	{
		if (const TArray<ObjectProperties::FAdmittedField>* Found = Cache.Find(&Class))
		{
			return *Found;
		}
		TArray<ObjectProperties::FAdmittedField> Fields = ObjectProperties::ListFields(Class);
		Fields.RemoveAll([bActor](const ObjectProperties::FAdmittedField& Field)
		{
			if (!Field.bWritable || Field.Property == nullptr)
			{
				return true;
			}
			// An actor's component pointers are listed as components, not as values.
			const FProperty* Inner = Field.Property;
			if (const FArrayProperty* Array = CastField<FArrayProperty>(Inner))
			{
				Inner = Array->Inner;
			}
			const FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(Inner);
			return bActor && Reference != nullptr && Reference->PropertyClass != nullptr
				&& Reference->PropertyClass->IsChildOf<UActorComponent>()
				&& Inner->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance);
		});
		return Cache.Add(&Class, MoveTemp(Fields));
	}

private:
	TMap<UClass*, TArray<ObjectProperties::FAdmittedField>> Cache;
};

TSharedRef<FJsonObject> PropertiesJson(UObject& Object, bool bActor, bool bDiffOnly, FFieldCache& Fields)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	for (const ObjectProperties::FAdmittedField& Field : Fields.Get(*Object.GetClass(), bActor))
	{
		// diff_only: struct members and array elements' members equal to the archetype's are left out as well.
		FAssetDocumentRuntimeValueDto Value;
		if (bDiffOnly ? ObjectProperties::ReadChanged(Object, Field, Value) : ObjectProperties::Read(Object, Field, Value))
		{
			if (TSharedPtr<FJsonValue> JsonValue = ProjectAssetDocumentRuntimeValue(Value))
			{
				Json->SetField(Field.Name, JsonValue);
			}
		}
	}
	return Json;
}

// The Details Transform row: the root component's relative property, read with the codec.
TSharedPtr<FJsonValue> TransformMember(USceneComponent& Root, const TCHAR* Property, TSharedRef<FJsonObject> Fallback)
{
	FAssetDocumentRuntimeValueDto Value;
	if (ObjectProperties::Read(Root, Property, Value))
	{
		if (TSharedPtr<FJsonValue> Json = ProjectAssetDocumentRuntimeValue(Value))
		{
			return Json;
		}
	}
	return MakeShared<FJsonValueObject>(Fallback);
}

TSharedPtr<FJsonValue> TransformJson(const AActor& Actor)
{
	USceneComponent* Root = Actor.GetRootComponent();
	if (Root == nullptr)
	{
		return NullValue();
	}
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	const FRotator Rotation = Root->GetRelativeRotation();
	Json->SetField(TEXT("location"), TransformMember(*Root, TEXT("RelativeLocation"), VectorJson(Root->GetRelativeLocation())));
	Json->SetField(TEXT("rotation"), TransformMember(*Root, TEXT("RelativeRotation"),
		NumbersJson({{TEXT("Pitch"), Rotation.Pitch}, {TEXT("Yaw"), Rotation.Yaw}, {TEXT("Roll"), Rotation.Roll}})));
	Json->SetField(TEXT("scale"), TransformMember(*Root, TEXT("RelativeScale3D"), VectorJson(Root->GetRelativeScale3D())));
	return MakeShared<FJsonValueObject>(Json);
}

const TCHAR* DescribeCreation(EComponentCreationMethod Method)
{
	switch (Method)
	{
	case EComponentCreationMethod::Native: return TEXT("Native");
	case EComponentCreationMethod::SimpleConstructionScript: return TEXT("Blueprint");
	case EComponentCreationMethod::UserConstructionScript: return TEXT("ConstructionScript");
	case EComponentCreationMethod::Instance:
	default: return TEXT("Instance");
	}
}

TArray<TSharedPtr<FJsonValue>> ComponentsJson(AActor& Actor, bool bProperties, bool bDiffOnly, FFieldCache& Fields)
{
	TInlineComponentArray<UActorComponent*> Components(&Actor);
	Components.RemoveAll([](const UActorComponent* Component)
	{
		return Component == nullptr || !IsValid(Component) || Component->IsVisualizationComponent();
	});
	USceneComponent* Root = Actor.GetRootComponent();
	Components.Sort([Root](const UActorComponent& Left, const UActorComponent& Right)
	{
		if ((&Left == Root) != (&Right == Root))
		{
			return &Left == Root;
		}
		return Left.GetName() < Right.GetName();
	});
	TArray<TSharedPtr<FJsonValue>> Result;
	for (UActorComponent* Component : Components)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Component->GetName());
		Json->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
		Json->SetStringField(TEXT("creation"), DescribeCreation(Component->CreationMethod));
		const USceneComponent* Scene = Cast<USceneComponent>(Component);
		const USceneComponent* Parent = Scene != nullptr ? Scene->GetAttachParent() : nullptr;
		Json->SetBoolField(TEXT("editable"), FComponentEditorUtils::CanEditComponentInstance(Component, Parent, false));
		const bool bSameActorParent = Parent != nullptr && Parent->GetOwner() == &Actor;
		Json->SetField(TEXT("parent"), bSameActorParent ? StringOrNull(Parent->GetName()) : NullValue());
		const FName Socket = Scene != nullptr ? Scene->GetAttachSocketName() : NAME_None;
		Json->SetField(TEXT("socket"), bSameActorParent && !Socket.IsNone() ? StringOrNull(Socket.ToString()) : NullValue());
		Json->SetBoolField(TEXT("root"), Component == Root);
		if (bProperties)
		{
			Json->SetObjectField(TEXT("properties"), PropertiesJson(*Component, false, bDiffOnly, Fields));
		}
		Result.Add(MakeShared<FJsonValueObject>(Json));
	}
	return Result;
}

TSharedRef<FJsonObject> LoadedActorJson(AActor& Actor, const FQueryRequest& Request, FFieldCache& Fields)
{
	TSharedRef<FJsonObject> Json = MakeActorRefJson(Actor);
	const FName Folder = Actor.GetFolderPath();
	Json->SetStringField(TEXT("folder"), Folder.IsNone() ? FString() : Folder.ToString());
	const AActor* Parent = Actor.GetAttachParentActor();
	Json->SetField(TEXT("parent"), Parent != nullptr ? StringOrNull(GetActorPath(*Parent)) : NullValue());
	const FName ParentSocket = Parent != nullptr ? Actor.GetAttachParentSocketName() : NAME_None;
	Json->SetField(TEXT("parent_socket"), ParentSocket.IsNone() ? NullValue() : StringOrNull(ParentSocket.ToString()));
	Json->SetArrayField(TEXT("tags"), StringArray(NamesToStrings(Actor.Tags)));
	Json->SetArrayField(TEXT("layers"), StringArray(NamesToStrings(Actor.Layers)));
	Json->SetArrayField(TEXT("data_layers"), StringArray(GetActorDataLayers(Actor)));
	Json->SetField(TEXT("transform"), TransformJson(Actor));
	if (!Request.Properties.IsEmpty())
	{
		TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Errors = MakeShared<FJsonObject>();
		for (const FString& Path : Request.Properties)
		{
			TSharedPtr<FJsonValue> Value;
			FString Error;
			if (Play::ReadActorValue(Actor, Path, Value, Error))
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
	if (Request.Detail != EQueryDetail::Summary)
	{
		const bool bProperties = Request.Detail == EQueryDetail::Properties;
		Json->SetArrayField(TEXT("components"), ComponentsJson(Actor, bProperties, Request.bDiffOnly, Fields));
		if (bProperties)
		{
			Json->SetObjectField(TEXT("properties"), PropertiesJson(Actor, true, Request.bDiffOnly, Fields));
		}
	}
	return Json;
}

TSharedRef<FJsonObject> UnloadedActorJson(const FUnloadedActor& Unloaded)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("path"), Unloaded.Path);
	Json->SetStringField(TEXT("guid"), FormatActorGuid(Unloaded.Guid));
	Json->SetStringField(TEXT("label"), Unloaded.Label);
	Json->SetStringField(TEXT("class"), Unloaded.ClassPath);
	Json->SetStringField(TEXT("level"), Unloaded.LevelPackage);
	Json->SetField(TEXT("external_package"), StringOrNull(Unloaded.ExternalPackage));
	Json->SetBoolField(TEXT("loaded"), false);
	Json->SetStringField(TEXT("folder"), Unloaded.Folder);
	Json->SetArrayField(TEXT("tags"), StringArray(NamesToStrings(Unloaded.Tags)));
	Json->SetArrayField(TEXT("data_layers"), StringArray(Unloaded.DataLayers));
	if (Unloaded.Bounds.IsValid)
	{
		TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetObjectField(TEXT("min"), VectorJson(Unloaded.Bounds.Min));
		Bounds->SetObjectField(TEXT("max"), VectorJson(Unloaded.Bounds.Max));
		Json->SetObjectField(TEXT("bounds"), Bounds);
	}
	else
	{
		Json->SetField(TEXT("bounds"), NullValue());
	}
	return Json;
}

bool MatchesFolder(const FString& ActorFolder, const FString& Filter)
{
	return ActorFolder == Filter || ActorFolder.StartsWith(Filter + TEXT("/"));
}

bool MatchesDataLayer(const TArray<FString>& DataLayers, const FString& Filter)
{
	for (const FString& DataLayer : DataLayers)
	{
		FString AssetName = FPackageName::ObjectPathToObjectName(DataLayer);
		if (DataLayer == Filter || AssetName == Filter)
		{
			return true;
		}
	}
	return false;
}

bool MatchesLocation(const FQueryRequest& Request, const FVector& Location)
{
	return (!Request.NearLocation.IsSet() || FVector::Dist(Location, Request.NearLocation.GetValue()) <= Request.NearRadius)
		&& (!Request.Bounds.IsSet() || Request.Bounds.GetValue().IsInsideOrOn(Location));
}

bool MatchesLoaded(const FQueryRequest& Request, AActor& Actor)
{
	if (Request.Class != nullptr && (Request.bExactClass ? Actor.GetClass() != Request.Class : !Actor.IsA(Request.Class)))
	{
		return false;
	}
	if (!Request.Name.IsEmpty() && !Actor.GetActorLabel(false).MatchesWildcard(Request.Name)
		&& !Actor.GetName().MatchesWildcard(Request.Name))
	{
		return false;
	}
	if (!Request.Folder.IsEmpty() && !MatchesFolder(Actor.GetFolderPath().ToString(), Request.Folder))
	{
		return false;
	}
	if (!Request.Tag.IsEmpty() && !Actor.Tags.Contains(FName(*Request.Tag)))
	{
		return false;
	}
	if (!Request.Layer.IsEmpty() && !Actor.Layers.Contains(FName(*Request.Layer)))
	{
		return false;
	}
	if (!Request.DataLayer.IsEmpty() && !MatchesDataLayer(GetActorDataLayers(Actor), Request.DataLayer))
	{
		return false;
	}
	if (!Request.Level.IsEmpty() && GetLevelPackageName(*Actor.GetLevel()) != Request.Level)
	{
		return false;
	}
	if (Request.bSelectedOnly && !Actor.IsSelected())
	{
		return false;
	}
	return MatchesLocation(Request, Actor.GetActorLocation());
}

bool MatchesUnloadedClass(const FQueryRequest& Request, const FUnloadedActor& Unloaded)
{
	if (Request.Class == nullptr)
	{
		return true;
	}
	const UClass* Loaded = FindObject<UClass>(nullptr, *Unloaded.ClassPath);
	const UClass* Class = Loaded != nullptr ? Loaded : Unloaded.NativeClass;
	if (Request.bExactClass)
	{
		return Unloaded.ClassPath == Request.Class->GetPathName();
	}
	return Class != nullptr && Class->IsChildOf(Request.Class);
}

bool MatchesUnloaded(const FQueryRequest& Request, const FUnloadedActor& Unloaded)
{
	return MatchesUnloadedClass(Request, Unloaded)
		&& (Request.Name.IsEmpty() || Unloaded.Label.MatchesWildcard(Request.Name) || Unloaded.Name.MatchesWildcard(Request.Name))
		&& (Request.Folder.IsEmpty() || MatchesFolder(Unloaded.Folder, Request.Folder))
		&& (Request.Tag.IsEmpty() || Unloaded.Tags.Contains(FName(*Request.Tag)))
		&& Request.Layer.IsEmpty()
		&& (Request.DataLayer.IsEmpty() || MatchesDataLayer(Unloaded.DataLayers, Request.DataLayer))
		&& (Request.Level.IsEmpty() || Unloaded.LevelPackage == Request.Level)
		&& !Request.bSelectedOnly
		&& MatchesLocation(Request, Unloaded.Location);
}

struct FCandidate
{
	TWeakObjectPtr<AActor> Actor;
	int32 UnloadedIndex = INDEX_NONE;
	FString LevelPackage;
	FString ObjectName;
};

FString GetObjectNameFromPath(const FString& Path)
{
	int32 Dot = INDEX_NONE;
	return Path.FindLastChar(TEXT('.'), Dot) ? Path.RightChop(Dot + 1) : Path;
}

TSharedRef<FJsonObject> WorldSummary(UWorld& World)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("world"), World.GetPathName());
	UWorldPartition* WorldPartition = World.GetWorldPartition();
	Json->SetBoolField(TEXT("partitioned"), WorldPartition != nullptr);
	Json->SetBoolField(TEXT("streaming"), WorldPartition != nullptr && WorldPartition->IsStreamingEnabled());
	const ULevel* Current = World.GetCurrentLevel();
	Json->SetStringField(TEXT("current_level"), Current != nullptr ? GetLevelPackageName(*Current) : FString());

	TArray<TSharedPtr<FJsonValue>> Levels;
	auto AddLevel = [&](ULevel* Level, const FString& Package, ULevelStreaming* Streaming)
	{
		TSharedRef<FJsonObject> LevelJson = MakeShared<FJsonObject>();
		LevelJson->SetStringField(TEXT("package"), Package);
		LevelJson->SetBoolField(TEXT("persistent"), Streaming == nullptr);
		LevelJson->SetBoolField(TEXT("current"), Level != nullptr && Level == Current);
		LevelJson->SetBoolField(TEXT("loaded"), Level != nullptr);
		LevelJson->SetBoolField(TEXT("visible"), Level != nullptr && Level->bIsVisible);
		LevelJson->SetBoolField(TEXT("locked"), Level != nullptr && FLevelUtils::IsLevelLocked(Level));
		int32 Count = 0;
		if (Level != nullptr)
		{
			for (AActor* Actor : Level->Actors)
			{
				Count += Actor != nullptr && IsListedActor(*Actor) ? 1 : 0;
			}
		}
		LevelJson->SetNumberField(TEXT("actor_count"), Count);
		LevelJson->SetField(TEXT("streaming_class"),
			Streaming != nullptr ? StringOrNull(Streaming->GetClass()->GetPathName()) : NullValue());
		Levels.Add(MakeShared<FJsonValueObject>(LevelJson));
	};
	if (World.PersistentLevel != nullptr)
	{
		AddLevel(World.PersistentLevel, GetLevelPackageName(*World.PersistentLevel), nullptr);
	}
	for (ULevelStreaming* Streaming : World.GetStreamingLevels())
	{
		if (Streaming != nullptr)
		{
			AddLevel(Streaming->GetLoadedLevel(), Streaming->GetWorldAssetPackageName(), Streaming);
		}
	}
	Json->SetArrayField(TEXT("levels"), Levels);

	TArray<FString> Folders;
	FActorFolders::Get().ForEachFolder(World, [&Folders](const FFolder& Folder)
	{
		Folders.Add(Folder.GetPath().ToString());
		return true;
	});
	Folders.Sort();
	Json->SetArrayField(TEXT("folders"), StringArray(Folders));

	TArray<FName> LayerNames;
	if (ULayersSubsystem* Layers = GEditor != nullptr ? GEditor->GetEditorSubsystem<ULayersSubsystem>() : nullptr)
	{
		Layers->AddAllLayerNamesTo(LayerNames);
	}
	TArray<FString> LayerStrings = NamesToStrings(LayerNames);
	LayerStrings.Sort();
	Json->SetArrayField(TEXT("layers"), StringArray(LayerStrings));

	TArray<TSharedPtr<FJsonValue>> DataLayers;
	if (const UDataLayerManager* Manager = WorldPartition != nullptr ? WorldPartition->GetDataLayerManager() : nullptr)
	{
		TArray<TSharedRef<FJsonObject>> Entries;
		Manager->ForEachDataLayerInstance([&Entries](UDataLayerInstance* Instance)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Instance->GetDataLayerShortName());
			const UDataLayerAsset* Asset = Instance->GetAsset();
			Entry->SetField(TEXT("asset"), Asset != nullptr ? StringOrNull(Asset->GetPathName()) : NullValue());
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
			Entry->SetStringField(TEXT("instance"), Instance->GetFName().ToString());
#else
			Entry->SetStringField(TEXT("instance"), Instance->GetDataLayerFName().ToString());
#endif
			Entries.Add(Entry);
			return true;
		});
		Algo::Sort(Entries, [](const TSharedRef<FJsonObject>& Left, const TSharedRef<FJsonObject>& Right)
		{
			return Left->GetStringField(TEXT("instance")) < Right->GetStringField(TEXT("instance"));
		});
		for (const TSharedRef<FJsonObject>& Entry : Entries)
		{
			DataLayers.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	Json->SetArrayField(TEXT("data_layers"), DataLayers);

	int32 Loaded = 0;
	ForEachListedActor(World, [&Loaded](AActor&) { ++Loaded; });
	Json->SetNumberField(TEXT("actor_count"), Loaded);
	Json->SetNumberField(TEXT("unloaded_actor_count"), GatherUnloadedActors(World).Num());
	// Unsaved changes of the open level (map, sublevel, World Partition actor and folder packages): what a
	// map change would refuse to lose (Level.UnsavedChangesWouldBeLost). Empty: everything is saved.
	TArray<FString> Dirty = GatherDirtyLevelPackages(World).Array();
	Dirty.Sort();
	Json->SetArrayField(TEXT("dirty_packages"), StringArray(Dirty));
	Json->SetBoolField(TEXT("never_saved"), World.GetPackage() != nullptr && !FPackageName::DoesPackageExist(World.GetPackage()->GetName()));
	return Json;
}
} // namespace

TSharedRef<FJsonObject> MakeLevelWorldSummary(UWorld& World)
{
	return WorldSummary(World);
}

bool CheckLevelToolAdmission(TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	if (GEditor == nullptr)
	{
		OutDiagnostics.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), TEXT("The editor is not available.")));
		return false;
	}
	if (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress())
	{
		OutDiagnostics.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.PlaySessionActive"),
			TEXT("A Play-In-Editor / Simulate session is running. The level tools work on the editor world only and are "
				"refused during a play session; stop it (play {\"action\": \"stop\"}) and retry. query_play reads the running game.")));
		return false;
	}
	if (GEditor->GetEditorWorldContext().World() == nullptr)
	{
		OutDiagnostics.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), TEXT("No editor world is open; retry shortly.")));
		return false;
	}
	return true;
}

FLevelToolResult RunLevelQuery(const TSharedPtr<FJsonObject>& RequestJson)
{
	check(IsInGameThread());
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	if (!CheckLevelToolAdmission(Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	FQueryRequest Request;
	if (!RequestJson.IsValid() || !ParseRequest(*RequestJson, Request, Diagnostics))
	{
		if (Diagnostics.IsEmpty())
		{
			Diagnostics.Add(InvalidRequest(TEXT(""), TEXT("The request must be a JSON object.")));
		}
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	UWorld& World = *GEditor->GetEditorWorldContext().World();
	if (Request.bWorldSummary)
	{
		return FLevelToolResult::Success(FString::Printf(TEXT("World %s"), *World.GetPathName()), WorldSummary(World));
	}

	TArray<FUnloadedActor> Unloaded;
	const bool bPartitioned = World.GetWorldPartition() != nullptr;
	if (bPartitioned && (Request.bIncludeUnloaded || Request.bHasActors))
	{
		Unloaded = GatherUnloadedActors(World);
	}

	TArray<FCandidate> Candidates;
	auto AddLoaded = [&Candidates](AActor& Actor)
	{
		FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.Actor = &Actor;
		Candidate.LevelPackage = GetLevelPackageName(*Actor.GetLevel());
		Candidate.ObjectName = Actor.GetName();
	};
	auto AddUnloaded = [&Candidates, &Unloaded](int32 Index)
	{
		FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.UnloadedIndex = Index;
		Candidate.LevelPackage = Unloaded[Index].LevelPackage;
		Candidate.ObjectName = GetObjectNameFromPath(Unloaded[Index].Path);
	};

	if (Request.bHasActors)
	{
		TSet<FString> Seen;
		for (int32 Index = 0; Index < Request.Actors.Num(); ++Index)
		{
			const FActorRefResolution Resolution = ResolveActorRef(World, Request.Actors[Index]);
			const FString Path = FString::Printf(TEXT("/actors/%d"), Index);
			if (!Resolution.bResolved)
			{
				TSharedPtr<FJsonObject> Details;
				if (!Resolution.Candidates.IsEmpty())
				{
					Details = MakeShared<FJsonObject>();
					Details->SetArrayField(TEXT("candidates"), StringArray(Resolution.Candidates));
				}
				Diagnostics.Add(MakeLevelDiagnostic(Path, Resolution.Code, Resolution.Message, Details));
				continue;
			}
			if (Seen.Contains(Resolution.Path))
			{
				continue;
			}
			Seen.Add(Resolution.Path);
			if (AActor* Actor = Resolution.Actor.Get())
			{
				if (MatchesLoaded(Request, *Actor))
				{
					AddLoaded(*Actor);
				}
			}
			else
			{
				const int32 UnloadedIndex = Unloaded.IndexOfByPredicate(
					[&Resolution](const FUnloadedActor& Entry) { return Entry.Guid == Resolution.Guid; });
				if (UnloadedIndex != INDEX_NONE && MatchesUnloaded(Request, Unloaded[UnloadedIndex]))
				{
					AddUnloaded(UnloadedIndex);
				}
			}
		}
		if (!Diagnostics.IsEmpty())
		{
			return FLevelToolResult::Failure(MoveTemp(Diagnostics));
		}
	}
	else
	{
		ForEachListedActor(World, [&](AActor& Actor)
		{
			if (MatchesLoaded(Request, Actor))
			{
				AddLoaded(Actor);
			}
		});
		if (Request.bIncludeUnloaded)
		{
			for (int32 Index = 0; Index < Unloaded.Num(); ++Index)
			{
				if (MatchesUnloaded(Request, Unloaded[Index]))
				{
					AddUnloaded(Index);
				}
			}
		}
	}

	// Stable order for paging: level package, then object name.
	Candidates.Sort([](const FCandidate& Left, const FCandidate& Right)
	{
		const int32 ByLevel = Left.LevelPackage.Compare(Right.LevelPackage, ESearchCase::CaseSensitive);
		return ByLevel != 0 ? ByLevel < 0 : Left.ObjectName.Compare(Right.ObjectName, ESearchCase::CaseSensitive) < 0;
	});

	FFieldCache Fields;
	TArray<TSharedPtr<FJsonValue>> Actors;
	const int32 First = FMath::Min(Request.Offset, Candidates.Num());
	const int32 Last = FMath::Min(Candidates.Num(), First + Request.Limit);
	for (int32 Index = First; Index < Last; ++Index)
	{
		const FCandidate& Candidate = Candidates[Index];
		if (AActor* Actor = Candidate.Actor.Get())
		{
			Actors.Add(MakeShared<FJsonValueObject>(LoadedActorJson(*Actor, Request, Fields)));
		}
		else if (Unloaded.IsValidIndex(Candidate.UnloadedIndex))
		{
			Actors.Add(MakeShared<FJsonValueObject>(UnloadedActorJson(Unloaded[Candidate.UnloadedIndex])));
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("world"), World.GetPathName());
	Payload->SetBoolField(TEXT("partitioned"), bPartitioned);
	Payload->SetNumberField(TEXT("total"), Candidates.Num());
	Payload->SetNumberField(TEXT("offset"), Request.Offset);
	Payload->SetNumberField(TEXT("limit"), Request.Limit);
	Payload->SetArrayField(TEXT("actors"), Actors);
	return FLevelToolResult::Success(FString::Printf(TEXT("%d of %d actors"), Actors.Num(), Candidates.Num()), Payload);
}

FLevelToolResult RunLevelQueryFromBody(const FString& RequestBody)
{
	if (RequestBody.TrimStartAndEnd().IsEmpty())
	{
		return RunLevelQuery(MakeShared<FJsonObject>());
	}
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		TArray<FAssetDocumentDiagnostic> Diagnostics;
		Diagnostics.Add(InvalidRequest(TEXT(""), FString::Printf(TEXT("The request body is not a JSON object: %s"),
			*Reader->GetErrorMessage())));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	return RunLevelQuery(Json);
}
}
