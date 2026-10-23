// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Sparse-over-defaults verification. Profiles whose extract leaves out values equal to their defaults, or
// that accept sparse documents (a struct written with only some members), must not report a VerificationMismatch when
// the document lists a default explicitly or writes a nested struct partially: the post-apply check and Diff compare
// the document with what Apply wrote, not with the shape of the read. Each test applies such documents through the
// production service (Create, saved) and expects the apply to succeed and Diff to be empty.
//
// UEAssetDoc.SparseDefaults.PhysicsAsset: shape fields at their defaults (Center, Rotation,
//   bContributeToMass), a body DefaultInstance default, a partial asset Details struct (SolverSettings).
// UEAssetDoc.SparseDefaults.Sequencer: a spawnable template with a component field at its default and a
//   partial engine struct with a member at its default; a Cine Camera focal length (derived field of view); spawnable and
//   template root fields the document names differently from the binding, at their defaults.
// UEAssetDoc.SparseDefaults.SoundCue: a partial struct in the cue Details and in node Details, node
//   Details at their defaults.
// UEAssetDoc.SparseDefaults.MetaSound: a partial struct in the source Details, an input Widget with
//   fields at their defaults.
// UEAssetDoc.SparseDefaults.Niagara: a partial struct in the system Details and in emitter Details,
//   renderer Details at their defaults with a partial struct.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentSparseDefaultsTests
{
const TCHAR* const FolderName = TEXT("AssetDocumentSparseDefaults");

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/")) + FolderName + TEXT("/") + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), FolderName, AssetName + TEXT(".assetdoc.json"));
	}

	// Saved assets are unloaded and deleted from disk as the Content Browser does.
	void Cleanup() const
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive())
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentSparseDefaultsTests", "Cleanup", "AssetDocument sparse defaults test cleanup"));
		}
		if (UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject())
		{
			Asset->GetOutermost()->ClearDirtyFlag();
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

inline FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

inline TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

inline FString DiagnosticsText(const FAssetDocumentResult& Result)
{
	TArray<FString> Lines;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Lines.Add(FString::Printf(TEXT("[%s] %s %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return Lines.IsEmpty() ? Result.Message : FString::Join(Lines, TEXT(" / "));
}

// Applies the document (saved) and expects success and an empty Diff afterwards.
inline bool ApplyAndCompare(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
{
	const TSharedPtr<FJsonObject> Document = Parse(Json);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the document parses"), *Stage), Document.IsValid()))
	{
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Serialize(Document), *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Applied = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the apply succeeds (%s)"), *Stage, *DiagnosticsText(Applied)), Applied.IsSuccess()))
	{
		return false;
	}
	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Document;
	const FAssetDocumentResult Diffed = FAssetDocumentModule::Get().GetService().Diff(DiffRequest);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: Diff succeeds (%s)"), *Stage, *DiagnosticsText(Diffed)),
		Diffed.IsSuccess() && Diffed.Payload.IsValid() && Diffed.Payload->TryGetArrayField(TEXT("changed"), Changed)))
	{
		return false;
	}
	return Test.TestEqual(*FString::Printf(TEXT("%s: Diff is empty (%s)"), *Stage, *Serialize(Diffed.Payload).Left(2000)), Changed->Num(), 0);
}

// One document per case, each on its own new asset: a failing case does not hide the others.
inline void RunCases(FAutomationTestBase& Test, const TCHAR* Prefix, TConstArrayView<TPair<const TCHAR*, const TCHAR*>> Cases)
{
	for (const TPair<const TCHAR*, const TCHAR*>& Case : Cases)
	{
		const FTarget Target(Prefix);
		ON_SCOPE_EXIT
		{
			Target.Cleanup();
		};
		// The case documents name their target as %TARGET%.
		ApplyAndCompare(Test, Target, FString(Case.Value).Replace(TEXT("%TARGET%"), *Target.PackageName), Case.Key);
	}
}
} // namespace AssetDocumentSparseDefaultsTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSparseDefaultsPhysicsAssetTest, "UEAssetDoc.SparseDefaults.PhysicsAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSparseDefaultsPhysicsAssetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSparseDefaultsTests;
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("shape fields at their defaults"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.PhysicsAsset",
			"Action": "Create", "FactoryContext": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
			"Body": {"Bodies": {"pelvis": {"Shapes": {"Capsules": [{"Center": {"X": 0, "Y": 0, "Z": 0}, "Rotation": {"Pitch": 0, "Yaw": 0, "Roll": 0},
				"Radius": 10, "Length": 20, "bContributeToMass": true}]}}}}})json")},
		{TEXT("body DefaultInstance with a default"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.PhysicsAsset",
			"Action": "Create", "FactoryContext": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
			"Body": {"Bodies": {"pelvis": {"Properties": {"DefaultInstance": {"bOverrideMass": true, "MassInKgOverride": 12, "LinearDamping": 0.01}},
				"Shapes": {"Capsules": [{"Radius": 10, "Length": 20}]}}}}})json")},
		{TEXT("partial asset Details struct"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.PhysicsAsset",
			"Action": "Create", "FactoryContext": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
			"Properties": {"SolverSettings": {"PositionIterations": 8}},
			"Body": {"Bodies": {"pelvis": {"Shapes": {"Capsules": [{"Radius": 10, "Length": 20}]}}}}})json")},
	};
	RunCases(*this, TEXT("PA_Sparse"), Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSparseDefaultsSequencerTest, "UEAssetDoc.SparseDefaults.Sequencer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSparseDefaultsSequencerTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSparseDefaultsTests;
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("template field at its default"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/LevelSequence.LevelSequence", "Action": "Create",
			"Target": "%TARGET%", "Properties": {}, "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}},
			"Bindings": [{"Name": "Cam", "Spawnable": {"Class": "/Script/Engine.CameraActor",
				"Template": {"Components": {"CameraComponent": {"FieldOfView": 90, "AspectRatio": 2}}}}}]}})json")},
		{TEXT("partial template struct with a default member"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/LevelSequence.LevelSequence",
			"Action": "Create", "Target": "%TARGET%", "Properties": {}, "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}},
			"Bindings": [{"Name": "Cam", "Spawnable": {"Class": "/Script/Engine.CameraActor",
				"Template": {"Components": {"CameraComponent": {"PostProcessSettings": {"bOverride_BloomIntensity": true, "BloomIntensity": 0.675}}}}}}]}})json")},
		{TEXT("partial template struct"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/LevelSequence.LevelSequence", "Action": "Create",
			"Target": "%TARGET%", "Properties": {}, "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}},
			"Bindings": [{"Name": "Cam", "Spawnable": {"Class": "/Script/Engine.CameraActor",
				"Template": {"Components": {"CameraComponent": {"PostProcessSettings": {"bOverride_BloomIntensity": true, "BloomIntensity": 2}}}}}}]}})json")},
		// A Cine Camera template's focal length recomputes the field of view, which its Details do not edit.
		{TEXT("Cine Camera focal length"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/LevelSequence.LevelSequence", "Action": "Create",
			"Target": "%TARGET%", "Properties": {}, "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}},
			"Bindings": [{"Name": "Cam", "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor",
				"Template": {"Components": {"CameraComponent": {"CurrentFocalLength": 24}}}}}],
			"Tracks": [{"Type": "CameraCut", "Sections": [{"Start": 0, "End": 150, "Properties": {"CameraBindingID": {"Binding": "Cam"}}}]}]}})json")},
		// Spawnable fields the document names differently from the binding (Ownership is SpawnOwnership), at their defaults.
		{TEXT("spawnable fields at their defaults"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/LevelSequence.LevelSequence", "Action": "Create",
			"Target": "%TARGET%", "Properties": {}, "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}},
			"Bindings": [{"Name": "Cam", "Spawnable": {"Class": "/Script/Engine.CameraActor", "Ownership": "InnerSequence", "ContinuouslyRespawn": false}}]}})json")},
		{TEXT("template root fields at their defaults"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/TemplateSequence.TemplateSequence", "Action": "Create",
			"Target": "%TARGET%", "Properties": {}, "Body": {"Root": {"Class": "/Script/Engine.CameraActor", "Ownership": "InnerSequence", "ContinuouslyRespawn": false}}})json")},
	};
	RunCases(*this, TEXT("LS_Sparse"), Cases);
	// A field the Cine Camera component's Details do not show (derived from the lens) is not a template field.
	{
		const FTarget Target(TEXT("LS_Sparse"));
		ON_SCOPE_EXIT
		{
			Target.Cleanup();
		};
		const TSharedPtr<FJsonObject> Document = Parse(FString(TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/LevelSequence.LevelSequence",
			"Action": "Create", "Target": "%TARGET%", "Properties": {}, "Body": {"Bindings": [{"Name": "Cam", "Spawnable": {
			"Class": "/Script/CinematicCamera.CineCameraActor", "Template": {"Components": {"CameraComponent": {"FieldOfView": 60}}}}}]}})json"))
			.Replace(TEXT("%TARGET%"), *Target.PackageName));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
		FFileHelper::SaveStringToFile(Serialize(Document), *Target.SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Target.SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Applied = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		TestTrue(*FString::Printf(TEXT("a Cine Camera FieldOfView is refused as not editable (%s)"), *DiagnosticsText(Applied)), !Applied.IsSuccess()
			&& DiagnosticsText(Applied).Contains(TEXT("Sequencer.FieldNotEditable")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSparseDefaultsSoundCueTest, "UEAssetDoc.SparseDefaults.SoundCue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSparseDefaultsSoundCueTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSparseDefaultsTests;
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("partial cue Details struct"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/Engine.SoundCue", "Action": "Create",
			"Target": "%TARGET%", "Properties": {"bOverrideAttenuation": true, "AttenuationOverrides": {"FalloffDistance": 2000}},
			"Body": {"Root": {"Input": "Wave"}, "Nodes": {"Wave": {"Class": "WavePlayer",
				"Properties": {"SoundWaveAssetPtr": "/Engine/VREditor/Sounds/UI/Click_on_Button.Click_on_Button"}}}}})json")},
		{TEXT("partial node Details struct"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/Engine.SoundCue", "Action": "Create",
			"Target": "%TARGET%", "Properties": {},
			"Body": {"Root": {"Input": "Att"}, "Nodes": {
				"Att": {"Class": "Attenuation", "Properties": {"bOverrideAttenuation": true, "AttenuationOverrides": {"FalloffDistance": 1500}}, "Inputs": ["Mod"]},
				"Mod": {"Class": "ModulatorContinuous", "Properties": {"PitchModulationParams": {"Default": 1.5}}, "Inputs": ["Wave"]},
				"Wave": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Engine/VREditor/Sounds/UI/Click_on_Button.Click_on_Button"}}}}})json")},
		{TEXT("node Details at their defaults"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/Engine.SoundCue", "Action": "Create",
			"Target": "%TARGET%", "Properties": {"VolumeMultiplier": 0.75},
			"Body": {"Root": {"Input": "Mod"}, "Nodes": {
				"Mod": {"Class": "Modulator", "Properties": {"PitchMin": 0.95, "PitchMax": 1.05, "VolumeMin": 0.95, "VolumeMax": 1.05}, "Inputs": ["Wave"]},
				"Wave": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Engine/VREditor/Sounds/UI/Click_on_Button.Click_on_Button",
					"bLooping": false}}}}})json")},
	};
	RunCases(*this, TEXT("SC_Sparse"), Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSparseDefaultsMetaSoundTest, "UEAssetDoc.SparseDefaults.MetaSound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSparseDefaultsMetaSoundTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSparseDefaultsTests;
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("partial source Details struct"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
			"Target": "%TARGET%", "Properties": {"OutputFormat": "Mono", "ModulationSettings": {"VolumeModulationDestination": {"Value": -6}}},
			"Body": {"Interfaces": ["UE.Source.OneShot"], "Graphs": {"Default": {"Nodes": {
				"Play": {"Class": "Input", "Member": "UE.Source.OnPlay"},
				"Osc": {"Class": "UE.Sine.Audio"},
				"Out": {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Osc"}},
				"Done": {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Inputs": {"UE.Source.OneShot.OnFinished": "Play"}}}}}}})json")},
		{TEXT("input Widget fields at their defaults"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
			"Target": "%TARGET%", "Properties": {"OutputFormat": "Mono"},
			"Body": {"Interfaces": ["UE.Source.OneShot"],
				"Inputs": {"Pitch": {"Type": "Float", "Default": 440.0, "Widget": {"Range": {"X": 100, "Y": 2000}, "WidgetType": "None", "ClampDefault": false}}},
				"Graphs": {"Default": {"Nodes": {
				"Play": {"Class": "Input", "Member": "UE.Source.OnPlay"},
				"Pitch": {"Class": "Input", "Member": "Pitch"},
				"Osc": {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
				"Out": {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Osc"}},
				"Done": {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Inputs": {"UE.Source.OneShot.OnFinished": "Play"}}}}}}})json")},
	};
	RunCases(*this, TEXT("MSS_Sparse"), Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSparseDefaultsNiagaraTest, "UEAssetDoc.SparseDefaults.Niagara",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSparseDefaultsNiagaraTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSparseDefaultsTests;
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("partial system Details struct"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Niagara.NiagaraSystem",
			"Action": "Create", "Definitions": {},
			"Properties": {"bFixedBounds": true, "FixedBounds": {"Min": {"X": -200, "Y": -200, "Z": -200}, "Max": {"X": 200, "Y": 200, "Z": 200}}},
			"Body": {"Emitters": [{"Name": "Jet", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain"}]}})json")},
		{TEXT("partial emitter Details struct"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Niagara.NiagaraSystem",
			"Action": "Create", "Definitions": {},
			"Body": {"Emitters": [{"Name": "Jet", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain",
				"Properties": {"Platforms": {"QualityLevelMask": 3}}}]}})json")},
		{TEXT("renderer Details at their defaults, partial struct"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Niagara.NiagaraSystem",
			"Action": "Create", "Definitions": {},
			"Body": {"Emitters": [{"Name": "Jet", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain",
				"Renderers": [{"Name": "NiagaraSpriteRendererProperties_0", "Class": "SpriteRenderer",
					"Properties": {"Alignment": "Unaligned", "SubImageSize": {"X": 1, "Y": 1}, "Platforms": {"QualityLevelMask": 3}}}]}]}})json")},
	};
	RunCases(*this, TEXT("NS_Sparse"), Cases);
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
