// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// The object property interface AssetDocumentEditor exports for the level tools
// (AssetDocumentEditorObjectProperties.h): instance admission, Details-equivalent
// writes that record into an editor transaction, and the level reference policy.

#include "AssetDocumentEditorObjectProperties.h"
#include "AssetDocumentTypes.h"
#include "Components/PointLightComponent.h"
#include "Components/RuntimeVirtualTextureComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/PointLight.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TargetPoint.h"
#include "LevelTestSupport.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"
#include "VT/RuntimeVirtualTextureVolume.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentLevelObjectPropertiesTests
{
using namespace AssetDocumentLevelTests;
namespace ObjectProperties = UE::AssetDocument::EditorObjectProperties;

FAssetDocumentRuntimeValueDto ToDto(const TSharedPtr<FJsonValue>& Json)
{
	FAssetDocumentRuntimeValueDto Value;
	switch (Json.IsValid() ? Json->Type : EJson::Null)
	{
	case EJson::Boolean:
		Value.Kind = EAssetDocumentRuntimeValueKind::Boolean;
		Value.BooleanValue = Json->AsBool();
		break;
	case EJson::Number:
	{
		const double Number = Json->AsNumber();
		if (FMath::IsFinite(Number) && FMath::Abs(Number) < 9.0e15 && FMath::RoundToDouble(Number) == Number)
		{
			Value.Kind = EAssetDocumentRuntimeValueKind::Integer;
			Value.IntegerValue = static_cast<int64>(Number);
		}
		else
		{
			Value.Kind = EAssetDocumentRuntimeValueKind::Number;
			Value.NumberValue = Number;
		}
		break;
	}
	case EJson::String:
		Value.Kind = EAssetDocumentRuntimeValueKind::String;
		Value.StringValue = Json->AsString();
		break;
	case EJson::Array:
		Value.Kind = EAssetDocumentRuntimeValueKind::Array;
		for (const TSharedPtr<FJsonValue>& Element : Json->AsArray())
		{
			Value.Elements.Add(ToDto(Element));
		}
		break;
	case EJson::Object:
		Value.Kind = EAssetDocumentRuntimeValueKind::Object;
		for (const auto& Member : Json->AsObject()->Values)
		{
			Value.ObjectKeys.Add(FString(*Member.Key));
			Value.Elements.Add(ToDto(Member.Value));
		}
		break;
	default:
		break;
	}
	return Value;
}

// A JSON value text ("1.5", "[\"a\"]", "{\"Z\": 5}") as a document value.
FAssetDocumentRuntimeValueDto Value(const FString& Text)
{
	const TSharedPtr<FJsonObject> Wrapper = ParseJsonObject(FString::Printf(TEXT("{\"v\": %s}"), *Text));
	return ToDto(Wrapper.IsValid() ? Wrapper->TryGetField(TEXT("v")) : nullptr);
}

FString ToJsonText(const FAssetDocumentRuntimeValueDto& Dto)
{
	const TSharedPtr<FJsonValue> Json = ProjectAssetDocumentRuntimeValue(Dto);
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Json, FString(), Writer);
	Writer->Close();
	return Text;
}

FString CodesOf(const TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	TArray<FString> Out;
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		Out.Add(Diagnostic.Path + TEXT(" ") + Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	return FString::Join(Out, TEXT(", "));
}

struct FDecodeOutcome
{
	bool bDecoded = false;
	ObjectProperties::FPreparedWrite Prepared;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;

	bool Has(const TCHAR* Code, const TCHAR* Path = nullptr) const
	{
		return Diagnostics.ContainsByPredicate([Code, Path](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code && (Path == nullptr || Diagnostic.Path == Path);
		});
	}

	FString Codes() const
	{
		TArray<FString> Out;
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
		{
			Out.Add(Diagnostic.Path + TEXT(" ") + Diagnostic.Code);
		}
		return FString::Join(Out, TEXT(", "));
	}
};

FDecodeOutcome Decode(UObject& Owner, const TCHAR* Property, const FString& Json,
	const ObjectProperties::FReferencePolicy& Policy = ObjectProperties::FReferencePolicy())
{
	FDecodeOutcome Outcome;
	Outcome.bDecoded = ObjectProperties::Decode(Owner, Property, Value(Json), Policy, Outcome.Prepared, Outcome.Diagnostics);
	return Outcome;
}

FString ReadJson(UObject& Owner, const TCHAR* Property)
{
	FAssetDocumentRuntimeValueDto Out;
	return ObjectProperties::Read(Owner, Property, Out) ? ToJsonText(Out) : FString(TEXT("<unreadable>"));
}

ObjectProperties::FReferencePolicy LevelPolicy(const ULevel& Level)
{
	ObjectProperties::FReferencePolicy Policy;
	Policy.SameLevel = &Level;
	return Policy;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelObjectPropertiesInstanceAdmissionTest,
	"UEAssetDoc.Level.ObjectProperties.InstanceAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelObjectPropertiesInstanceAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelObjectPropertiesTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Props"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	const TWeakObjectPtr<APointLight> Light =
		Cast<APointLight>(SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(10, 20, 30), TEXT("Lamp")));
	if (!TestTrue(TEXT("light spawned"), Light.IsValid()))
	{
		return false;
	}
	const TWeakObjectPtr<UPointLightComponent> Component = Light->PointLightComponent;
	const ObjectProperties::FReferencePolicy Policy = LevelPolicy(*World->PersistentLevel);
	const float IntensityBefore = Component->Intensity;

	TestTrue(TEXT("archetype value before the edit"), ObjectProperties::IsIdenticalToArchetype(*Component, TEXT("Intensity")));
	{
		FDecodeOutcome Outcome = Decode(*Component, TEXT("Intensity"), TEXT("1234.5"), Policy);
		TestTrue(*FString::Printf(TEXT("Intensity decodes (%s)"), *Outcome.Codes()), Outcome.bDecoded);
		TestEqual(TEXT("decode writes nothing"), Component->Intensity, IntensityBefore);
		TestEqual(TEXT("prepared owner"), Outcome.Prepared.GetOwner(), static_cast<UObject*>(Component.Get()));
		TestEqual(TEXT("prepared property"), Outcome.Prepared.GetPropertyName(), FString(TEXT("Intensity")));
		const int32 QueueBefore = GEditor->Trans->GetQueueLength();
		FAssetDocumentRuntimeValueDto Readback;
		TArray<FAssetDocumentRuntimeDiagnostic> CommitDiagnostics;
		{
			const FScopedTransaction Transaction(FText::FromString(TEXT("Set intensity")));
			TestTrue(TEXT("commit"), ObjectProperties::Commit(Outcome.Prepared, CommitDiagnostics, &Readback));
		}
		TestFalse(TEXT("commit consumes the prepared write"), Outcome.Prepared.IsValid());
		TestEqual(TEXT("readback"), ToJsonText(Readback), FString(TEXT("1234.5")));
		TestEqual(TEXT("stored"), Component->Intensity, 1234.5f);
		TestEqual(TEXT("read"), ReadJson(*Component, TEXT("Intensity")), FString(TEXT("1234.5")));
		TestFalse(TEXT("now an instance override"), ObjectProperties::IsIdenticalToArchetype(*Component, TEXT("Intensity")));
		TestTrue(TEXT("other fields stay archetype values"), ObjectProperties::IsIdenticalToArchetype(*Component, TEXT("AttenuationRadius")));
		TestEqual(TEXT("the write recorded into the transaction"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		TestEqual(TEXT("Undo restores the value"), Component->Intensity, IntensityBefore);
	}

	// Struct values are edits: omitted members keep their value.
	{
		FDecodeOutcome Outcome = Decode(*Component, TEXT("RelativeLocation"), TEXT("{\"Z\": 50}"), Policy);
		TestTrue(*FString::Printf(TEXT("partial struct decodes (%s)"), *Outcome.Codes()), Outcome.bDecoded);
		TArray<FAssetDocumentRuntimeDiagnostic> CommitDiagnostics;
		TestTrue(TEXT("partial struct commits"), ObjectProperties::Commit(Outcome.Prepared, CommitDiagnostics));
		TestEqual(TEXT("omitted members keep their value"), Light->GetActorLocation(), FVector(10, 20, 50));
	}

	// EditDefaultsOnly: read-only on instances.
	{
		const FDecodeOutcome Level = Decode(*Light, TEXT("bAlwaysRelevant"), TEXT("true"), Policy);
		TestFalse(TEXT("EditDefaultsOnly is refused on an instance"), Level.bDecoded);
		TestTrue(*FString::Printf(TEXT("Level.PropertyNotEditableOnInstance (%s)"), *Level.Codes()),
			Level.Has(TEXT("Level.PropertyNotEditableOnInstance"), TEXT("/bAlwaysRelevant")));
		const FDecodeOutcome Asset = Decode(*Light, TEXT("bAlwaysRelevant"), TEXT("true"));
		TestTrue(*FString::Printf(TEXT("without a level policy: Generic.NonWritableProperty (%s)"), *Asset.Codes()),
			Asset.Has(TEXT("Generic.NonWritableProperty"), TEXT("/bAlwaysRelevant")));
		TestEqual(TEXT("still readable"), ReadJson(*Light, TEXT("bAlwaysRelevant")), FString(TEXT("false")));
	}
	const TArray<ObjectProperties::FAdmittedField> Fields = ObjectProperties::ListFields(*APointLight::StaticClass());
	const ObjectProperties::FAdmittedField* AlwaysRelevant =
		Fields.FindByPredicate([](const ObjectProperties::FAdmittedField& Field) { return Field.Name == TEXT("bAlwaysRelevant"); });
	if (TestNotNull(TEXT("ListFields lists read-only fields"), AlwaysRelevant))
	{
		TestFalse(TEXT("not writable"), AlwaysRelevant->bWritable);
		TestTrue(TEXT("editable on templates"), AlwaysRelevant->bEditableOnTemplates);
		TestEqual(TEXT("codec"), AlwaysRelevant->CodecId, FString(TEXT("bool.v1")));
	}
	TestTrue(TEXT("ListWritable lists Intensity"), ObjectProperties::ListWritable(*Component).ContainsByPredicate(
		[](const ObjectProperties::FAdmittedField& Field) { return Field.Name == TEXT("Intensity") && Field.bWritable; }));
	TestFalse(TEXT("ListWritable leaves out read-only fields"), ObjectProperties::ListWritable(*Light).ContainsByPredicate(
		[](const ObjectProperties::FAdmittedField& Field) { return Field.Name == TEXT("bAlwaysRelevant"); }));

	TestTrue(TEXT("unknown property"), Decode(*Component, TEXT("NoSuchProperty"), TEXT("1"), Policy)
		.Has(TEXT("Generic.UnknownProperty"), TEXT("/NoSuchProperty")));
	const FDecodeOutcome TypeError = Decode(*Component, TEXT("Intensity"), TEXT("\"bright\""), Policy);
	TestFalse(TEXT("a type error fails"), TypeError.bDecoded);
	TestTrue(*FString::Printf(TEXT("a codec code (%s)"), *TypeError.Codes()), TypeError.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code.StartsWith(TEXT("TypedValueCodec.")) || Diagnostic.Code == TEXT("PropertyWrite.DecodeFailed");
		}));
	TestEqual(TEXT("nothing written by failed decodes"), Component->Intensity, IntensityBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelObjectPropertiesActorReferencesTest,
	"UEAssetDoc.Level.ObjectProperties.ActorReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelObjectPropertiesActorReferencesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelObjectPropertiesTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Refs"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	ULevel* Sub = Maps.AddSublevel(*World, TEXT("RefsSub"));
	if (!TestNotNull(TEXT("sublevel created"), Sub))
	{
		return false;
	}
	ULevel& Persistent = *World->PersistentLevel;
	const TWeakObjectPtr<ASceneCapture2D> Capture =
		Cast<ASceneCapture2D>(SpawnActor(Persistent, ASceneCapture2D::StaticClass(), FVector::ZeroVector, TEXT("Capture")));
	const TWeakObjectPtr<AActor> Same = SpawnActor(Persistent, ATargetPoint::StaticClass(), FVector(100, 0, 0), TEXT("Same"));
	const TWeakObjectPtr<AActor> Other = SpawnActor(*Sub, ATargetPoint::StaticClass(), FVector(200, 0, 0), TEXT("Other"));
	const TWeakObjectPtr<ARuntimeVirtualTextureVolume> Volume = Cast<ARuntimeVirtualTextureVolume>(
		SpawnActor(Persistent, ARuntimeVirtualTextureVolume::StaticClass(), FVector::ZeroVector, TEXT("Volume")));
	if (!TestTrue(TEXT("actors spawned"), Capture.IsValid() && Same.IsValid() && Other.IsValid() && Volume.IsValid()))
	{
		return false;
	}
	const TWeakObjectPtr<USceneCaptureComponent2D> CaptureComponent = Capture->GetCaptureComponent2D();
	const ObjectProperties::FReferencePolicy Policy = LevelPolicy(Persistent);
	const FString SamePath = FSoftObjectPath(Same.Get()).ToString();
	const FString OtherPath = FSoftObjectPath(Other.Get()).ToString();

	{
		FDecodeOutcome Outcome = Decode(*CaptureComponent, TEXT("HiddenActors"),
			FString::Printf(TEXT("[\"%s\"]"), *SamePath), Policy);
		TestTrue(*FString::Printf(TEXT("a same-level actor reference decodes (%s)"), *Outcome.Codes()), Outcome.bDecoded);
		TArray<FAssetDocumentRuntimeDiagnostic> CommitDiagnostics;
		const bool bCommitted = ObjectProperties::Commit(Outcome.Prepared, CommitDiagnostics);
		TestTrue(*FString::Printf(TEXT("and commits (%s)"), *CodesOf(CommitDiagnostics)), bCommitted);
		TestTrue(TEXT("stored"), CaptureComponent->HiddenActors.Num() == 1 && CaptureComponent->HiddenActors[0] == Same.Get());
		TestEqual(TEXT("read back as the actor path"), ReadJson(*CaptureComponent, TEXT("HiddenActors")),
			FString::Printf(TEXT("[\"%s\"]"), *SamePath));
	}
	{
		const FDecodeOutcome Outcome = Decode(*CaptureComponent, TEXT("HiddenActors"),
			FString::Printf(TEXT("[\"%s\", \"%s\"]"), *SamePath, *OtherPath), Policy);
		TestFalse(TEXT("a hard reference to another level is refused"), Outcome.bDecoded);
		TestTrue(*FString::Printf(TEXT("Level.CrossLevelHardReference at the element (%s)"), *Outcome.Codes()),
			Outcome.Has(TEXT("Level.CrossLevelHardReference"), TEXT("/HiddenActors/1")));
	}
	{
		// (A reference the property already holds is kept by the asset rule; name a new one.)
		const FDecodeOutcome Outcome = Decode(*CaptureComponent, TEXT("HiddenActors"),
			FString::Printf(TEXT("[\"%s\", \"%s\"]"), *SamePath, *FSoftObjectPath(Volume.Get()).ToString()));
		TestFalse(TEXT("asset semantics refuse level actors"), Outcome.bDecoded);
		TestTrue(*FString::Printf(TEXT("Property.EditorDerivedReference (%s)"), *Outcome.Codes()),
			Outcome.Has(TEXT("Property.EditorDerivedReference")));
	}

	// Soft references may cross levels.
	const TWeakObjectPtr<URuntimeVirtualTextureComponent> VolumeComponent = Volume->VirtualTextureComponent;
	if (TestTrue(TEXT("volume component"), VolumeComponent.IsValid()))
	{
		FDecodeOutcome Outcome = Decode(*VolumeComponent, TEXT("BoundsAlignActor"), FString::Printf(TEXT("\"%s\""), *OtherPath), Policy);
		TestTrue(*FString::Printf(TEXT("a soft cross-level reference decodes (%s)"), *Outcome.Codes()), Outcome.bDecoded);
		TArray<FAssetDocumentRuntimeDiagnostic> CommitDiagnostics;
		const bool bCommitted = ObjectProperties::Commit(Outcome.Prepared, CommitDiagnostics);
		TestTrue(*FString::Printf(TEXT("and commits (%s)"), *CodesOf(CommitDiagnostics)), bCommitted);
		TestEqual(TEXT("stored"), ReadJson(*VolumeComponent, TEXT("BoundsAlignActor")), FString::Printf(TEXT("\"%s\""), *OtherPath));
	}

	// "@id" names an object created earlier in the same request.
	ObjectProperties::FReferencePolicy BatchPolicy = Policy;
	BatchPolicy.ResolveBatchId = [Same](FStringView Id) -> UObject*
	{
		return Id == TEXT("t1") ? Same.Get() : nullptr;
	};
	{
		FDecodeOutcome Outcome = Decode(*CaptureComponent, TEXT("HiddenActors"), TEXT("[\"@t1\"]"), BatchPolicy);
		TestTrue(*FString::Printf(TEXT("@t1 resolves (%s)"), *Outcome.Codes()), Outcome.bDecoded);
		TArray<FAssetDocumentRuntimeDiagnostic> CommitDiagnostics;
		CaptureComponent->HiddenActors.Reset();
		const bool bCommitted = ObjectProperties::Commit(Outcome.Prepared, CommitDiagnostics);
		TestTrue(*FString::Printf(TEXT("and commits (%s)"), *CodesOf(CommitDiagnostics)), bCommitted);
		TestTrue(TEXT("stored the resolved actor"), CaptureComponent->HiddenActors.Num() == 1 && CaptureComponent->HiddenActors[0] == Same.Get());
	}
	{
		const FDecodeOutcome Outcome = Decode(*CaptureComponent, TEXT("HiddenActors"), TEXT("[\"@nope\"]"), BatchPolicy);
		TestFalse(TEXT("an unknown @id fails"), Outcome.bDecoded);
		TestTrue(*FString::Printf(TEXT("Level.UnknownId (%s)"), *Outcome.Codes()), Outcome.Has(TEXT("Level.UnknownId"), TEXT("/HiddenActors/0")));
	}
	{
		FDecodeOutcome Outcome = Decode(*Same, TEXT("Tags"), TEXT("[\"@t1\"]"), BatchPolicy);
		TestTrue(*FString::Printf(TEXT("a string value \"@t1\" is not a reference (%s)"), *Outcome.Codes()), Outcome.bDecoded);
		TArray<FAssetDocumentRuntimeDiagnostic> CommitDiagnostics;
		const bool bCommitted = ObjectProperties::Commit(Outcome.Prepared, CommitDiagnostics);
		TestTrue(*FString::Printf(TEXT("and commits (%s)"), *CodesOf(CommitDiagnostics)), bCommitted);
		TestTrue(TEXT("stored as written"), Same->Tags.Num() == 1 && Same->Tags[0] == FName(TEXT("@t1")));
	}
	return true;
}
#endif
