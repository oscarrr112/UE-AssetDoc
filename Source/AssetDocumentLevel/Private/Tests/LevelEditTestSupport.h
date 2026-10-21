// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentEditorObjectProperties.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentTypes.h"
#include "AssetDocumentLevelActorRef.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "LevelEditTransaction.h"
#include "LevelEditValues.h"
#include "LevelEditorViewport.h"
#include "LevelTestSupport.h"
#include "LevelWorldIndex.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "PackageTools.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentLevelEditTests
{
using namespace AssetDocumentLevelTests;
using UE::AssetDocumentLevel::Private::FLevelToolResult;
using UE::AssetDocumentLevel::GetActorPath;

// Runs one edit_level request as the HTTP route does (game thread) and logs the outcome.
inline FLevelToolResult Edit(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	const TSharedPtr<FJsonObject> Request = ParseJsonObject(Json);
	Test.TestTrue(*FString::Printf(TEXT("%s: the request is JSON"), Stage), Request.IsValid());
	const FLevelToolResult Result = UE::AssetDocumentLevel::Private::RunLevelEdit(Request);
	Test.AddInfo(FString::Printf(TEXT("%s: %d %s"), Stage, Result.HttpStatus, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s:   [%s %s] %s %s"), Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path,
			*Diagnostic.Message));
	}
	return Result;
}

inline bool HasCode(const FLevelToolResult& Result, const TCHAR* Code, const TCHAR* Path = nullptr)
{
	return Result.Diagnostics.ContainsByPredicate([Code, Path](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && (Path == nullptr || Diagnostic.Path == Path);
	});
}

inline const FAssetDocumentDiagnostic* FindCode(const FLevelToolResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

inline FString PayloadString(const FLevelToolResult& Result, const TCHAR* Field)
{
	FString Value;
	if (Result.Payload.IsValid())
	{
		Result.Payload->TryGetStringField(Field, Value);
	}
	return Value;
}

inline bool PayloadBool(const FLevelToolResult& Result, const TCHAR* Field)
{
	bool bValue = false;
	if (Result.Payload.IsValid())
	{
		Result.Payload->TryGetBoolField(Field, bValue);
	}
	return bValue;
}

inline TArray<FString> PayloadStrings(const FLevelToolResult& Result, const TCHAR* Field)
{
	TArray<FString> Values;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(Field, Array))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			Values.Add(Value->AsString());
		}
	}
	return Values;
}

inline TArray<TSharedPtr<FJsonValue>> Changes(const FLevelToolResult& Result)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	return Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changes"), Array) ? *Array : TArray<TSharedPtr<FJsonValue>>();
}

// The change entry for the op pointer Op ("/ops/1/components/LightComponent0/properties/Intensity").
inline TSharedPtr<FJsonObject> FindChange(const FLevelToolResult& Result, const FString& Op)
{
	for (const TSharedPtr<FJsonValue>& Change : Changes(Result))
	{
		if (Change->AsObject()->GetStringField(TEXT("op")) == Op)
		{
			return Change->AsObject();
		}
	}
	return nullptr;
}

// Compact JSON text for comparisons. Whole numbers are written as integers (the value
// projection writes a stored float 20 as "2e+01" or "20.0"); other numbers as the
// shortest decimal that reads back.
inline FString JsonText(const TSharedPtr<FJsonValue>& Value)
{
	return UE::AssetDocumentLevel::Private::LevelValueToText(UE::AssetDocumentLevel::Private::JsonToLevelValue(
		Value.IsValid() ? Value : MakeShared<FJsonValueNull>()));
}

inline AActor* FindActorByPath(const FString& Path)
{
	AActor* Actor = Path.IsEmpty() ? nullptr : Cast<AActor>(FSoftObjectPath(Path).ResolveObject());
	return Actor != nullptr && UE::AssetDocumentLevel::Private::IsListedActor(*Actor) ? Actor : nullptr;
}

// The path of the actor a batch created under id ("payload.actors.<id>.path").
inline FString BatchPath(const FLevelToolResult& Result, const TCHAR* Id)
{
	const TSharedPtr<FJsonObject>* Actors = nullptr;
	const TSharedPtr<FJsonObject>* Actor = nullptr;
	FString Path;
	if (Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("actors"), Actors)
		&& (*Actors)->TryGetObjectField(Id, Actor))
	{
		(*Actor)->TryGetStringField(TEXT("path"), Path);
	}
	return Path;
}

inline AActor* BatchActor(const FLevelToolResult& Result, const TCHAR* Id)
{
	return FindActorByPath(BatchPath(Result, Id));
}

// A property value of an object in document form, as compact JSON ("<unreadable>" when not).
inline FString ReadValue(UObject* Object, const TCHAR* Property)
{
	UE::AssetDocumentLevel::Private::FLevelValue Value;
	if (Object == nullptr || !UE::AssetDocument::EditorObjectProperties::Read(*Object, Property, Value))
	{
		return TEXT("<unreadable>");
	}
	// A string (an object or actor path, a name) as itself, anything else as JSON text.
	return Value.Kind == EAssetDocumentRuntimeValueKind::String ? Value.StringValue
		: JsonText(UE::AssetDocumentLevel::Private::LevelValueToJson(Value));
}

inline TArray<FString> ListedActorPaths(ULevel& Level)
{
	TArray<FString> Paths;
	for (AActor* Actor : Level.Actors)
	{
		if (Actor != nullptr && UE::AssetDocumentLevel::Private::IsListedActor(*Actor))
		{
			Paths.Add(UE::AssetDocumentLevel::GetActorPath(*Actor));
		}
	}
	Paths.Sort();
	return Paths;
}

inline int32 UndoQueueLength()
{
	return GEditor->Trans->GetQueueLength();
}

inline void ResetUndo()
{
	GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentLevelEditTests", "Reset", "edit_level test"));
}

// The editor's description of the newest Undo entry ("" when there is none).
inline FString NewestUndoTitle()
{
	const int32 Length = GEditor->Trans->GetQueueLength();
	const int32 UndoCount = GEditor->Trans->GetUndoCount();
	const FTransaction* Transaction = Length - UndoCount > 0 ? GEditor->Trans->GetTransaction(Length - UndoCount - 1) : nullptr;
	return Transaction != nullptr ? Transaction->GetTitle().ToString() : FString();
}

// An AssetDocument-created asset for a test (a Blueprint the level is built with), in
// its own folder; unloaded and deleted from disk on destruction.
class FScopedTestAsset
{
public:
	explicit FScopedTestAsset(const TCHAR* Name)
		: PackageName(FString::Printf(TEXT("/Game/__AssetDocumentTests__/LevelAssets/A%s/%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12), Name))
	{
	}

	~FScopedTestAsset()
	{
		if (GEditor != nullptr)
		{
			ResetUndo();
		}
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
			UPackageTools::UnloadPackages({Package});
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		const FString Directory = FPaths::GetPath(FPackageName::LongPackageNameToFilename(PackageName, TEXT("")));
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.*"), true, false, false);
		// Only package files go to the registry (the sidecar .assetdoc.json is not a package name).
		Files.RemoveAll([](const FString& File)
		{
			return !FPackageName::IsPackageExtension(*FPaths::GetExtension(File, true));
		});
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		if (!Files.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		}
	}

	FScopedTestAsset(const FScopedTestAsset&) = delete;
	FScopedTestAsset& operator=(const FScopedTestAsset&) = delete;

	// Applies an AssetDocument (its Target is this package) as the AI would, saved.
	bool Apply(FAutomationTestBase& Test, const FString& Document, const TCHAR* Stage) const
	{
		return Test.TestTrue(*FString::Printf(TEXT("%s: AssetDocument apply"), Stage), ApplyResult(Test, Document, Stage).IsSuccess());
	}

	FAssetDocumentResult ApplyResult(FAutomationTestBase& Test, const FString& Document, const TCHAR* Stage) const
	{
		const FString Sidecar = FPackageName::LongPackageNameToFilename(PackageName, TEXT(".assetdoc.json"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Sidecar), true);
		if (!FFileHelper::SaveStringToFile(Document, *Sidecar))
		{
			Test.AddError(FString::Printf(TEXT("%s: could not write %s"), Stage, *Sidecar));
			return FAssetDocumentResult::Failure(TEXT("sidecar not written"));
		}
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Sidecar;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		Log(Test, Result, Stage);
		return Result;
	}

	// validate_asset_document of an inline document.
	FAssetDocumentResult Validate(FAutomationTestBase& Test, const FString& Document, const TCHAR* Stage) const
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = ParseJsonObject(Document);
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
		Log(Test, Result, Stage);
		return Result;
	}

	static void Log(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const TCHAR* Stage)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s:   [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
	}

	FString ObjectPath() const
	{
		return PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	}

	FString ClassPath() const
	{
		return ObjectPath() + TEXT("_C");
	}

	UBlueprint* GetBlueprint() const
	{
		return LoadObject<UBlueprint>(nullptr, *ObjectPath());
	}

	const FString PackageName;
};

// The test door: a Blueprint with an instance-editable variable, a variable that is not
// instance editable, an actor reference variable and two Blueprint (SCS) components.
inline FString DoorBlueprintDocument(const FString& PackageName, const TCHAR* Action, double BrightnessDefault)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",")
		TEXT("\"Variables\":[")
		TEXT("{\"Name\":\"Brightness\",\"Type\":\"float\",\"DefaultValue\":%g,\"InstanceEditable\":true},")
		TEXT("{\"Name\":\"bStartsOpen\",\"Type\":\"bool\",\"DefaultValue\":true,\"InstanceEditable\":true},")
		TEXT("{\"Name\":\"Secret\",\"Type\":\"float\",\"DefaultValue\":2.0,\"InstanceEditable\":false},")
		TEXT("{\"Name\":\"LinkedLight\",\"Type\":\"object:/Script/Engine.Actor\",\"InstanceEditable\":true}],")
		TEXT("\"Components\":[")
		TEXT("{\"Name\":\"Frame\",\"Class\":\"/Script/Engine.StaticMeshComponent\",\"Properties\":{\"StaticMesh\":\"/Engine/BasicShapes/Cube.Cube\"}},")
		TEXT("{\"Name\":\"Lamp\",\"Class\":\"/Script/Engine.PointLightComponent\",\"AttachTo\":\"Frame\",\"Properties\":{\"Intensity\":1000.0}}]}}"),
		*PackageName, Action, BrightnessDefault);
}

// A Camera Animation Sequence create as an AI writes it: the default Cine Camera root with a small
// Transform shake.
inline FString CameraAnimationDocument(const FString& PackageName)
{
	return FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/TemplateSequence.CameraAnimationSequence",
		"Action": "Create", "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 30}},
		"Root": {"Class": "/Script/CinematicCamera.CineCameraActor", "Tracks": [{"Type": "Transform", "Sections": [{"Channels": {
			"Location.Y": {"Keys": [{"Frame": 0, "Value": 0}, {"Frame": 15, "Value": 2}, {"Frame": 30, "Value": 0}]},
			"Rotation.X": {"Keys": [{"Frame": 0, "Value": 0}, {"Frame": 10, "Value": 0.5}, {"Frame": 30, "Value": 0}]}}}]}]}}})"),
		*PackageName);
}

// The editor's Camera Animation Sequence factory makes its default Cine Camera through the actor
// factory in the world of the current level editing viewport (spawned transient, then destroyed).
// Without a level editor viewport (automation without Slate) that path is skipped, so a test makes
// one current, as the trial's editor had.
class FScopedLevelEditingViewport
{
public:
	FScopedLevelEditingViewport()
		: Previous(GCurrentLevelEditingViewportClient)
	{
		if (GCurrentLevelEditingViewportClient == nullptr)
		{
			const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
			if (!Clients.IsEmpty())
			{
				GCurrentLevelEditingViewportClient = Clients[0];
			}
			else
			{
				Owned = MakeUnique<FLevelEditorViewportClient>(nullptr);
				GCurrentLevelEditingViewportClient = Owned.Get();
			}
		}
	}

	~FScopedLevelEditingViewport()
	{
		GCurrentLevelEditingViewportClient = Previous;
	}

private:
	FLevelEditorViewportClient* Previous = nullptr;
	TUniquePtr<FLevelEditorViewportClient> Owned;
};
}
#endif
