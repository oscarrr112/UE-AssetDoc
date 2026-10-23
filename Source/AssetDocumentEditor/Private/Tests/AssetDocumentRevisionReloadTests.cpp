// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The KI-17 audit: BaseRevision is the hash of the asset's default extract. A property the engine derives when the
// package loads (PostLoad) but the apply does not set would change it on the next load although nobody changed the
// asset (a false Document.StaleBase; MetaSound bLooping was one). For one asset per profile: apply a Create document
// (saved; a Skeleton, an AnimSequence and a NiagaraEmitter cannot be created by a document, so an /Engine one is
// duplicated and updated), take the revision, unload the package and load it from disk, and take it again; the two must
// be equal. On a difference the test names the extract paths that changed. One was found: a camera cut (or sub / shot,
// CVar, template sequence) section's Blend, set by its constructor in its track (NewSectionBlend).
//   UEAssetDoc.RevisionReload.<Area>

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
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentRevisionReloadTestsLocal
{
const TCHAR* const FolderName = TEXT("AssetDocumentRevisionReload");

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
		PackageFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), FolderName, AssetName + TEXT(".assetdoc.json")));
	}

	// Saved assets are unloaded and deleted from disk as the Content Browser does.
	void Cleanup() const
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive())
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentRevisionReloadTests", "Cleanup", "AssetDocument revision reload test cleanup"));
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

FString SerializeJson(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

FString SerializeJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	FString Text;
	if (Value.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Value, FString(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonObject> ParseJson(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

FString ResultText(const FAssetDocumentResult& Result)
{
	TArray<FString> Lines;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Lines.Add(FString::Printf(TEXT("[%s] %s %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return Lines.IsEmpty() ? Result.Message : FString::Join(Lines, TEXT(" / "));
}

// The default extract (what BaseRevision hashes) and its revision.
FString ExtractRevision(const FString& ObjectPath, TSharedPtr<FJsonObject>& OutRead, FString& OutError)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	Request.bIncludeBaseRevision = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	FString Revision;
	OutRead = Result.Payload;
	if (!Result.IsSuccess() || !OutRead.IsValid() || !OutRead->TryGetStringField(TEXT("BaseRevision"), Revision))
	{
		OutError = ResultText(Result);
	}
	return Revision;
}

// JSON paths whose values differ (at most Limit).
void CollectDifferences(const TSharedPtr<FJsonValue>& Before, const TSharedPtr<FJsonValue>& After, const FString& Path, TArray<FString>& Out, int32 Limit = 12)
{
	if (Out.Num() >= Limit)
	{
		return;
	}
	const bool bBoth = Before.IsValid() && After.IsValid();
	if (bBoth && Before->Type == EJson::Object && After->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> A = Before->AsObject();
		const TSharedPtr<FJsonObject> B = After->AsObject();
		TSet<FString> Keys;
		for (const auto& Field : A->Values) { Keys.Add(FString(*Field.Key)); }
		for (const auto& Field : B->Values) { Keys.Add(FString(*Field.Key)); }
		for (const FString& Key : Keys)
		{
			CollectDifferences(AssetDocumentServerJson::FindFieldRef(*A, Key), AssetDocumentServerJson::FindFieldRef(*B, Key), Path + TEXT("/") + Key, Out, Limit);
		}
		return;
	}
	if (bBoth && Before->Type == EJson::Array && After->Type == EJson::Array && Before->AsArray().Num() == After->AsArray().Num())
	{
		for (int32 Index = 0; Index < Before->AsArray().Num(); ++Index)
		{
			CollectDifferences(Before->AsArray()[Index], After->AsArray()[Index], FString::Printf(TEXT("%s/%d"), *Path, Index), Out, Limit);
		}
		return;
	}
	const FString A = SerializeJsonValue(Before);
	const FString B = SerializeJsonValue(After);
	if (A != B)
	{
		Out.Add(FString::Printf(TEXT("%s: %s -> %s"), *Path, *A.Left(160), *B.Left(160)));
	}
}

struct FCase
{
	const TCHAR* Label;
	// The document applied (saved) to the target, naming it %TARGET%.
	const TCHAR* Document;
	// Optional: the target starts as a Duplicate of this asset (the document is then an Update).
	const TCHAR* DuplicateOf = nullptr;
	// Optional: an asset made first (its own case, any depth); the document names its object path %DEP%.
	const FCase* Dependency = nullptr;
};

FString CaseDocument(const FCase& Case, const FTarget& Target, const FString& Dependency)
{
	return FString(Case.Document).Replace(TEXT("%TARGET%"), *Target.PackageName).Replace(TEXT("%DEP%"), *Dependency);
}

// Makes the case's asset (its dependencies first, each on its own target in Made), saved.
bool MakeAsset(FAutomationTestBase& Test, const FCase& Case, const FTarget& Target, TArray<TUniquePtr<FTarget>>& Made)
{
	FString Dependency;
	if (Case.Dependency != nullptr)
	{
		const FTarget& Before = *Made.Add_GetRef(MakeUnique<FTarget>(TEXT("RRDep")));
		if (!MakeAsset(Test, *Case.Dependency, Before, Made))
		{
			return false;
		}
		Dependency = Before.ObjectPath;
	}
	if (Case.DuplicateOf != nullptr)
	{
		FAssetDocumentDuplicateRequest Request;
		Request.Source = Case.DuplicateOf;
		Request.Target = Target.PackageName;
		const FAssetDocumentResult Duplicated = FAssetDocumentModule::Get().GetService().Duplicate(Request);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: %s duplicates (%s)"), Case.Label, Case.DuplicateOf, *ResultText(Duplicated)), Duplicated.IsSuccess()))
		{
			return false;
		}
	}
	const TSharedPtr<FJsonObject> Document = ParseJson(CaseDocument(Case, Target, Dependency));
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the document parses"), Case.Label), Document.IsValid()))
	{
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(SerializeJson(Document), *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Applied = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	return Test.TestTrue(*FString::Printf(TEXT("%s: the document applies (%s)"), Case.Label, *ResultText(Applied)), Applied.IsSuccess());
}

// Apply (saved) -> revision -> unload the package, load it from disk -> revision; equal, or the paths that changed.
void RunCase(FAutomationTestBase& Test, const FCase& Case)
{
	TArray<TUniquePtr<FTarget>> Made;
	const FTarget& Target = *Made.Add_GetRef(MakeUnique<FTarget>(TEXT("RR")));
	ON_SCOPE_EXIT
	{
		// Dependents first.
		for (int32 Index = 0; Index < Made.Num(); ++Index)
		{
			Made[Index]->Cleanup();
		}
	};
	if (!MakeAsset(Test, Case, Target, Made))
	{
		return;
	}
	TSharedPtr<FJsonObject> BeforeRead;
	FString Error;
	const FString Before = ExtractRevision(Target.ObjectPath, BeforeRead, Error);
	if (!Test.TestFalse(*FString::Printf(TEXT("%s: the applied asset has a revision (%s)"), Case.Label, *Error), Before.IsEmpty()))
	{
		return;
	}
	UObject* Asset = FSoftObjectPath(Target.ObjectPath).ResolveObject();
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the apply saved the asset"), Case.Label),
		Asset != nullptr && !Asset->GetOutermost()->IsDirty() && IFileManager::Get().FileExists(*Target.PackageFile)))
	{
		return;
	}
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive())
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentRevisionReloadTests", "Reload", "AssetDocument revision reload"));
	}
	TArray<UPackage*> Packages = {Asset->GetOutermost()};
	Asset = nullptr;
	FText UnloadError;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the package unloads (%s)"), Case.Label, *UnloadError.ToString()),
		UPackageTools::UnloadPackages(Packages, UnloadError, true)))
	{
		return;
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: the package loads from disk"), Case.Label),
		LoadObject<UObject>(nullptr, *Target.ObjectPath, nullptr, LOAD_NoWarn)))
	{
		return;
	}
	TSharedPtr<FJsonObject> AfterRead;
	const FString After = ExtractRevision(Target.ObjectPath, AfterRead, Error);
	if (Before != After)
	{
		TArray<FString> Differences;
		CollectDifferences(MakeShared<FJsonValueObject>(BeforeRead), MakeShared<FJsonValueObject>(AfterRead), FString(), Differences);
		Test.AddError(FString::Printf(TEXT("%s: BaseRevision changed on reload (%s -> %s): %s"), Case.Label, *Before, *After,
			*FString::Join(Differences, TEXT(" | "))));
	}
	else
	{
		Test.AddInfo(FString::Printf(TEXT("%s: BaseRevision %s survives the reload"), Case.Label, *Before));
	}
}

void RunCases(FAutomationTestBase& Test, TConstArrayView<FCase> Cases)
{
	for (const FCase& Case : Cases)
	{
		RunCase(Test, Case);
	}
}
} // namespace AssetDocumentRevisionReloadTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadBlueprintTest, "UEAssetDoc.RevisionReload.Blueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	const FCase Cases[] = {
		{TEXT("Blueprint"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.Blueprint", "Action": "Create",
			"Body": {"ParentClass": "/Script/Engine.Actor",
				"Variables": [{"Name": "Brightness", "Type": "float", "DefaultValue": 2.5, "Category": "Look"}, {"Name": "bRefresh", "Type": "bool", "DefaultValue": true}],
				"Components": [{"Name": "Root", "Class": "/Script/Engine.SceneComponent"},
					{"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent", "AttachTo": "Root", "Properties": {"StaticMesh": "/Engine/BasicShapes/Cube.Cube"}}],
				"EventDispatchers": [{"Name": "OnDied"}],
				"FunctionGraphs": [{"Name": "UserConstructionScript", "Nodes": [{"Id": "GetMesh", "Action": "K2:Get:Mesh"},
					{"Id": "MakeMid", "Action": "K2:CallFunction:/Script/Engine.PrimitiveComponent:CreateDynamicMaterialInstance"}, {"Id": "Last", "Action": "K2:PrintString"}],
					"Links": [{"From": "Entry:then", "To": "MakeMid:execute"}, {"From": "GetMesh:Mesh", "To": "MakeMid:self"}, {"From": "MakeMid:then", "To": "Last:execute"}]}]}})json")},
		{TEXT("WidgetBlueprint"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/UMGEditor.WidgetBlueprint", "Action": "Create",
			"Body": {"ParentClass": "/Script/UMG.UserWidget", "Variables": [{"Name": "Count", "Type": "int"}],
				"WidgetTree": {"RootWidget": {"Name": "Root", "Class": "/Script/UMG.CanvasPanel", "Children": [
					{"Name": "Title", "Class": "/Script/UMG.TextBlock", "IsVariable": true, "Properties": {"Text": "Welcome"}},
					{"Name": "Go", "Class": "/Script/UMG.Button", "IsVariable": true}]}}}})json")},
		{TEXT("AnimBlueprint"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.AnimBlueprint", "Action": "Create",
			"Body": {"ParentClass": "/Script/Engine.AnimInstance", "TargetSkeleton": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton",
				"Preview": {"Mesh": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP"}, "Variables": [{"Name": "Speed", "Type": "float"}],
				"AnimGraph": {"Nodes": [{"Id": "Idle", "Action": "Anim:SequencePlayer", "Properties": {"Node.Sequence": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle"}}],
					"Links": [{"From": "Idle:Pose", "To": "Output:Result"}]}}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadAudioTest, "UEAssetDoc.RevisionReload.Audio",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadAudioTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	const FCase Cases[] = {
		{TEXT("SoundCue"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.SoundCue", "Action": "Create",
			"Properties": {"VolumeMultiplier": 0.8, "bOverrideAttenuation": true},
			"Body": {"Root": {"Input": "Tail"}, "Nodes": {
				"Tail": {"Class": "Concatenator", "Inputs": ["Shot", "Surface"], "Properties": {"InputVolume": [1.0, 0.6]}},
				"Shot": {"Class": "Modulator", "Inputs": ["Variations"], "Properties": {"PitchMin": 0.92, "PitchMax": 1.08}},
				"Variations": {"Class": "Random", "Inputs": ["ShotA", "ShotB"], "Properties": {"Weights": [1.0, 0.5]}},
				"Surface": {"Class": "Switch", "Inputs": ["ShotA", "ShotB"], "Properties": {"IntParameterName": "Surface"}},
				"ShotA": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess"}},
				"ShotB": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Engine/VREditor/Sounds/UI/Click_on_Button.Click_on_Button"},
					"Comment": "rare", "CommentVisible": true}},
				"Comments": {"Layers": {"Text": "Shot layers", "Position": [-1300, -300], "Size": [500, 800], "FontSize": 24}}}})json")},
		{TEXT("MetaSoundSource"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Target": "%TARGET%", "Action": "Create",
			"Properties": {"OutputFormat": "Mono"}, "Body": {"Interfaces": ["UE.Source.OneShot"],
				"Inputs": {"Pitch": {"Type": "Float", "Default": 440.0, "Widget": {"Range": {"X": 100, "Y": 2000}}}},
				"Graphs": {"Default": {"Nodes": {"Play": {"Class": "Input", "Member": "UE.Source.OnPlay"}, "Pitch": {"Class": "Input", "Member": "Pitch"},
					"Osc": {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
					"Env": {"Class": "AD Envelope.AD Envelope.Audio", "Inputs": {"Trigger": "Play"}, "Defaults": {"Decay Time": 0.3}},
					"Amp": {"Class": "UE.Multiply.Audio", "Inputs": {"PrimaryOperand": "Osc", "AdditionalOperands": "Env.Out Envelope"}},
					"Out": {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Amp"}},
					"Done": {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Inputs": {"UE.Source.OneShot.OnFinished": "Env.On Done"}}}}}}})json")},
		{TEXT("MetaSoundSource (looping)"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
			"Target": "%TARGET%", "Properties": {"OutputFormat": "Mono"}, "Body": {"Interfaces": []}})json")},
		{TEXT("MetaSoundPatch"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundPatch", "Action": "Create", "Target": "%TARGET%",
			"Body": {"Metadata": {"Description": "Gain stage", "Referenceable": true}, "Inputs": {"In": {"Type": "Audio"}, "Gain": {"Type": "Float", "Default": 0.5}},
				"Outputs": {"Out": {"Type": "Audio"}}, "Graphs": {"Default": {"Nodes": {"In": {"Class": "Input", "Member": "In", "Position": [-400, 0]},
					"Gain": {"Class": "Input", "Member": "Gain", "Position": [-400, 120]},
					"Mul": {"Class": "UE.Multiply.Audio by Float", "Position": [-150, 0], "Inputs": {"PrimaryOperand": "In", "AdditionalOperands": "Gain"}},
					"Out": {"Class": "Output", "Member": "Out", "Position": [100, 0], "Inputs": {"Out": "Mul"}}}}}}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadNiagaraTest, "UEAssetDoc.RevisionReload.Niagara",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadNiagaraTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	const FCase Cases[] = {
		{TEXT("NiagaraSystem"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {},
			"Properties": {"bDeterminism": true, "RandomSeed": 7},
			"Body": {"UserParameters": [{"Name": "User.Rate", "Type": "float", "Value": 40.0, "Category": "Spawn"}],
				"Emitters": [{"Name": "Jet", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain", "Properties": {"bLocalSpace": true},
					"Renderers": [{"Name": "Glow", "Class": "LightRenderer", "Properties": {"RadiusScale": 3.0}},
						{"Name": "NiagaraSpriteRendererProperties_0", "Class": "SpriteRenderer", "Properties": {"Alignment": "VelocityAligned"}}]},
					{"Name": "Blank", "From": "Empty"}]}})json")},
		{TEXT("NiagaraEmitter"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Niagara.NiagaraEmitter", "Action": "Update", "Definitions": {},
			"Properties": {"bIsInheritable": true}})json"), TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain")},
		{TEXT("NiagaraScript"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Niagara.NiagaraScript", "Action": "Create", "Definitions": {},
			"Properties": {}, "Body": {"Usage": "Module"}})json")},
		{TEXT("NiagaraParameterCollection"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Niagara.NiagaraParameterCollection", "Action": "Create",
			"Definitions": {}, "Properties": {}, "Body": {"Parameters": [{"Name": "Speed", "Type": "float", "Value": 2.5},
				{"Name": "Tint", "Type": "LinearColor", "Value": {"R": 1.0, "G": 0.5, "B": 0.25, "A": 1.0}}, {"Name": "Count", "Type": "int32"},
				{"Name": "Curve", "Type": "/Script/Niagara.NiagaraDataInterfaceCurve"}]}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadPhysicsAssetTest, "UEAssetDoc.RevisionReload.PhysicsAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadPhysicsAssetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	const FCase Cases[] = {
		{TEXT("PhysicsAsset"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/Engine.PhysicsAsset", "Action": "Create", "Target": "%TARGET%",
			"FactoryContext": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP", "Properties": {"SolverSettings": {"PositionIterations": 8}},
			"Body": {"Bodies": {"pelvis": {"Fit": {}}, "spine_03": {"Fit": {"GeomType": "EFG_Sphere"}}, "head": {"Shapes": {"Spheres": [{"Radius": 10}]}}}}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadRigTest, "UEAssetDoc.RevisionReload.Rig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadRigTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	static const FCase IKRig = {TEXT("IKRig (retargeter source)"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/IKRig.IKRigDefinition", "Action": "Create", "Target": "%TARGET%",
			"Body": {"PreviewMesh": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
				"Goals": {"LeftFootIK": {"Bone": "foot_l"}, "RightHandIK": {"Bone": "hand_r", "RotationAlpha": 0.5, "Offset": {"Location": {"X": 0, "Y": 0, "Z": 10}}}},
				"Solvers": [{"Type": "FullBodyIK", "StartBone": "pelvis", "Settings": {"Iterations": 30}, "Goals": {"LeftFootIK": {}},
					"Bones": {"calf_l": {"X": "Limited", "MinX": -10, "MaxX": 10}}}, {"Type": "LimbIK", "StartBone": "upperarm_r", "Goals": {"RightHandIK": {}}}],
				"RetargetRoot": "pelvis", "Chains": {"Spine": {"Start": "spine_01", "End": "spine_03"}, "LeftLeg": {"Start": "thigh_l", "End": "foot_l", "Goal": "LeftFootIK"},
					"RightArm": {"Start": "upperarm_r", "End": "hand_r", "Goal": "RightHandIK"}}}})json")};
	const FCase Cases[] = {
		{TEXT("IKRigDefinition"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/IKRig.IKRigDefinition", "Action": "Create", "Target": "%TARGET%",
			"Body": {"PreviewMesh": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
				"Goals": {"LeftFootIK": {"Bone": "foot_l"}, "RightHandIK": {"Bone": "hand_r", "RotationAlpha": 0.5, "Offset": {"Location": {"X": 0, "Y": 0, "Z": 10}}}},
				"Solvers": [{"Type": "FullBodyIK", "StartBone": "pelvis", "Settings": {"Iterations": 30}, "Goals": {"LeftFootIK": {}},
					"Bones": {"calf_l": {"X": "Limited", "MinX": -10, "MaxX": 10}}}, {"Type": "LimbIK", "StartBone": "upperarm_r", "Goals": {"RightHandIK": {}}}],
				"RetargetRoot": "pelvis", "Chains": {"Spine": {"Start": "spine_01", "End": "spine_03"}, "LeftLeg": {"Start": "thigh_l", "End": "foot_l", "Goal": "LeftFootIK"},
					"RightArm": {"Start": "upperarm_r", "End": "hand_r", "Goal": "RightHandIK"}}}})json")},
		{TEXT("IKRetargeter"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/IKRig.IKRetargeter", "Action": "Create", "Target": "%TARGET%",
			"Body": {"Source": {"IKRig": "%DEP%"}, "Target": {"IKRig": "%DEP%"}, "Ops": [{"Name": "Pelvis Motion", "Type": "PelvisMotion",
				"Settings": {"TranslationAlpha": 0.9}}, {"Name": "FK Chains", "Type": "FKChains"}, {"Name": "Run IK Rig", "Type": "RunIKRig",
				"Children": [{"Name": "IK Chains", "Type": "IKChains"}]}, {"Name": "Curves", "Type": "CurveRemap"}]}})json"), nullptr, &IKRig},
		{TEXT("ControlRigBlueprint"), TEXT(R"json({"SchemaVersion": 1, "Action": "Create", "Properties": {}, "Class": "/Script/ControlRigDeveloper.ControlRigBlueprint",
			"Target": "%TARGET%", "Body": {"Variables": [{"Name": "Speed", "Type": "float", "Default": 2.0}],
				"Graphs": {"Rig": {"Nodes": {"Begin": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]},
					"Note": {"Comment": "rig", "Position": [0, -200], "Size": [400, 150], "Color": {"R": 0.2, "G": 0.4, "B": 0.8, "A": 1}}}}},
				"Functions": {"Read Speed": {"Public": true, "Category": "Motion", "Description": "The speed, doubled.",
					"Inputs": [{"Name": "ExecuteContext", "Type": "/Script/RigVM.RigVMExecuteContext", "Direction": "IO"}, {"Name": "Gain", "Type": "float", "Default": 2.0}],
					"Outputs": [{"Name": "Value", "Type": "float"}], "LocalVariables": [{"Name": "Bias", "Type": "float", "Default": 0.25}],
					"Nodes": {"Get": {"Variable": "Speed", "Position": [0, 100]}, "GetBias": {"Variable": "Bias", "Position": [0, 250]},
						"Times": {"Unit": "RigVMFunction_MathFloatMul", "Position": [200, 100], "Inputs": {"A": "Get.Value", "B": "Entry.Gain"}},
						"Plus": {"Unit": "RigVMFunction_MathFloatAdd", "Position": [400, 100], "Inputs": {"A": "Times.Result", "B": "GetBias.Value"}}},
					"Order": ["Get", "GetBias", "Times", "Plus"],
					"Return": {"Position": [800, 0], "Inputs": {"ExecuteContext": "Entry.ExecuteContext", "Value": "Plus.Result"}}}}}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadAITest, "UEAssetDoc.RevisionReload.AI",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadAITest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	const FCase Cases[] = {
		{TEXT("BehaviorTree"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/AIModule.BehaviorTree", "Action": "Create", "Properties": {},
			"Body": {"BlackboardAsset": null, "Tree": {"GraphGuid": "0000000000000000000000000000a100", "Comments": [],
				"Root": {"Id": "0000000000000000000000000000a001", "Class": "/Script/AIModule.BTComposite_Sequence",
					"Children": [{"Id": "0000000000000000000000000000a002", "Class": "/Script/AIModule.BTTask_Wait"}]}}}})json")},
		{TEXT("BlackboardData"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/AIModule.BlackboardData", "Action": "Create", "Definitions": {},
			"Properties": {}, "Body": {"Parent": null, "Keys": [{"Category": "RoundTrip", "Description": "persisted",
				"KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Int"}, "KeyTypeProperties": {"DefaultValue": 7},
				"Name": "RoundTripKey", "bInstanceSynced": false}]}})json")},
		{TEXT("EnvQuery"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/AIModule.EnvQuery", "Action": "Create", "Definitions": {}, "Properties": {},
			"Body": {"Options": [{"Key": "Points", "Generator": {"Class": "ActorsOfClass", "Properties": {"SearchedActorClass": "/Script/Engine.TargetPoint",
				"SearchRadius": {"DefaultValue": 3000.0}}}, "Tests": [{"Key": "Nearest", "Class": "Distance", "Properties": {"TestPurpose": "Score",
				"ScoringEquation": "InverseLinear"}}]}]}})json")},
		{TEXT("StateTree"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/StateTreeModule.StateTree", "Action": "Create", "Properties": {},
			"Body": {"Schema": {"Class": "/Script/GameplayStateTreeModule.StateTreeComponentSchema"}, "States": [{"Name": "Root", "Children": [
				{"Name": "Idle", "Tasks": [{"Type": "/Script/StateTreeModule.StateTreeDelayTask", "Name": "Wait", "Instance": {"RandomDeviation": 0.5}}],
					"Transitions": [{"Trigger": "OnStateCompleted", "To": "Root/Walk"}]}, {"Name": "Walk"}]}]}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadDataTest, "UEAssetDoc.RevisionReload.Data",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadDataTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	static const FCase Table = {TEXT("DataTable (parent)"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.DataTable",
		"Action": "Create", "Properties": {}, "Body": {"RowStruct": "/Script/AssetDocumentTests.AssetDocumentDataTestRow",
		"Rows": [{"Name": "Sword", "Values": {"Count": 1}}, {"Name": "Bow", "Values": {"Count": 2}}]}})json")};
	static const FCase Curves = {TEXT("CurveTable (parent)"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.CurveTable",
		"Action": "Create", "Properties": {}, "Body": {"CurveTableMode": "SimpleCurves", "Rows": [{"Name": "Damage", "Keys": [{"Time": 0, "Value": 2}]}]}})json")};
	const FCase Cases[] = {
		{TEXT("DataTable"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.DataTable", "Action": "Create",
			"Properties": {"bStripFromClientBuilds": true}, "Body": {"RowStruct": "/Script/AssetDocumentTests.AssetDocumentDataTestRow",
				"Rows": [{"Name": "A", "Values": {"Count": 1}}, {"Name": "B", "Values": {"Count": 5}}]}})json")},
		{TEXT("CompositeDataTable"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.CompositeDataTable", "Action": "Create",
			"Properties": {}, "Body": {"RowStruct": "/Script/AssetDocumentTests.AssetDocumentDataTestRow", "ParentTables": ["%DEP%"]}})json"), nullptr, &Table},
		{TEXT("CurveTable"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.CurveTable", "Action": "Create", "Properties": {},
			"Body": {"CurveTableMode": "SimpleCurves", "Rows": [{"Name": "Damage", "InterpMode": "Linear", "Keys": [{"Time": 1, "Value": 10}, {"Time": 10, "Value": 55.5}]},
				{"Name": "Speed", "InterpMode": "Constant", "Keys": [{"Time": 0, "Value": 0.3}, {"Time": 5, "Value": 0.7}], "PostInfinityExtrap": "Linear"}]}})json")},
		{TEXT("CompositeCurveTable"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.CompositeCurveTable", "Action": "Create",
			"Properties": {}, "Body": {"ParentTables": ["%DEP%"]}})json"), nullptr, &Curves},
		{TEXT("StringTable"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.StringTable", "Action": "Create", "Properties": {},
			"Body": {"Namespace": "UI", "Entries": {"Menu.Start": "Start Game", "Menu.Quit": "Quit"}, "MetaData": {"Menu.Start": {"Comment": "Main menu button"}}}})json")},
		{TEXT("UserDefinedEnum"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.UserDefinedEnum", "Action": "Create", "Properties": {},
			"Body": {"Tooltip": "How an item is used.", "Bitflags": false, "Enumerators": [{"Name": "Melee", "Tooltip": "Close range"}, {"Name": "Bow"}, {"Name": "Magic"}]}})json")},
		{TEXT("UserDefinedStruct"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/CoreUObject.UserDefinedStruct", "Action": "Create", "Properties": {},
			"Body": {"Tooltip": "One inventory item.", "Members": [{"Name": "Label", "Type": "text", "DefaultValue": "Sword", "Tooltip": "Shown in the UI", "MultiLine": true},
				{"Name": "Damage", "Type": "float", "DefaultValue": 12.5, "ClampMin": "0", "UIMax": "100"},
				{"Name": "Count", "Type": "int", "DefaultValue": 3, "SaveGame": true, "InstanceEditable": false},
				{"Name": "Offset", "Type": "struct:/Script/CoreUObject.Vector", "DefaultValue": {"X": 1, "Y": 2, "Z": 3}, "Widget3D": true},
				{"Name": "Tags", "Type": "array:name", "DefaultValue": ["A", "B"]}, {"Name": "Lookup", "Type": {"Category": "name", "Container": "map", "ValueType": "int"},
				"DefaultValue": {"K": 1}}, {"Name": "Icon", "Type": "softobject:/Script/Engine.Texture2D", "DefaultValue": "/Engine/EngineResources/DefaultTexture.DefaultTexture"}]}})json")},
		{TEXT("MirrorDataTable"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.MirrorDataTable", "Action": "Create", "Properties": {},
			"Body": {}, "FactoryContext": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton"})json")},
		{TEXT("CurveFloat (generic)"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "AssetType": "GenericAsset", "Class": "/Script/Engine.CurveFloat", "Action": "Create",
			"Properties": {"bIsEventCurve": true, "FloatCurve": {"Keys": [{"Time": 0, "Value": 1.5}, {"Time": 2, "Value": -3.25}], "PostInfinityExtrap": "Cycle"}}})json")},
		{TEXT("DataAsset (generic)"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "AssetType": "GenericAsset", "Class": "/Script/AssetDocumentTests.TestDataAsset", "Action": "Create",
			"Properties": {"TestString": "made", "TestInt": 42, "TestFloat": 3, "bTestBool": true, "TestName": "CreatedName"}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadMaterialTest, "UEAssetDoc.RevisionReload.Material",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadMaterialTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	static const FCase Parent = {TEXT("Material (parent)"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.Material",
		"Action": "Create", "Properties": {}, "Body": {"Expressions": {"Rough": {"Class": "ScalarParameter", "Position": [-400, 0],
		"Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5}}, "Tint": {"Class": "VectorParameter", "Position": [-400, 300],
		"Properties": {"ParameterName": "Tint"}}}, "Outputs": {"Roughness": "Rough", "BaseColor": "Tint.RGB"}}})json")};
	const FCase Cases[] = {
		{TEXT("Material"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.Material", "Action": "Create", "Properties": {"TwoSided": true},
			"Body": {"Expressions": {"Rough": {"Class": "ScalarParameter", "Position": [-400, 0], "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5}},
				"Color": {"Class": "VectorParameter", "Position": [-400, 200], "Properties": {"ParameterName": "Color"}},
				"Tex": {"Class": "TextureSampleParameter2D", "Position": [-400, 400], "Properties": {"ParameterName": "Albedo",
					"Texture": "/Engine/EngineResources/DefaultTexture.DefaultTexture"}}},
				"Outputs": {"Roughness": "Rough", "BaseColor": "Color.RGB", "Normal": "Tex.RGB"}}})json")},
		{TEXT("MaterialFunction"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.MaterialFunction", "Action": "Create", "Properties": {},
			"Body": {"Expressions": {"Value": {"Class": "FunctionInput", "Position": [-500, 0], "Properties": {"InputName": "Value", "InputType": "FunctionInput_Vector3"}},
				"Mul": {"Class": "Multiply", "Position": [-250, 0], "Properties": {"ConstB": 2}, "Inputs": {"A": "Value"}},
				"Result": {"Class": "FunctionOutput", "Position": [0, 0], "Properties": {"OutputName": "Result"}, "Inputs": {"A": "Mul"}}}}})json")},
		{TEXT("MaterialInstanceConstant"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.MaterialInstanceConstant", "Action": "Create",
			"Properties": {}, "Body": {"Parent": "%DEP%", "Parameters": {"Roughness": 0.25, "Tint": [1, 0.25, 0.125, 1]}}})json"), nullptr, &Parent},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadSequencerTest, "UEAssetDoc.RevisionReload.Sequencer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadSequencerTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	const FCase Cases[] = {
		{TEXT("LevelSequence"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/LevelSequence.LevelSequence", "Action": "Create", "Target": "%TARGET%", "Properties": {},
			"Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}}, "Bindings": [{"Name": "Cam", "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor",
				"Template": {"Components": {"CameraComponent": {"CurrentFocalLength": 24}}}}}],
				"Tracks": [{"Type": "CameraCut", "Sections": [{"Start": 0, "End": 150, "Properties": {"CameraBindingID": {"Binding": "Cam"}}}]}]}})json")},
		{TEXT("TemplateSequence"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/TemplateSequence.TemplateSequence", "Action": "Create", "Target": "%TARGET%",
			"Properties": {}, "Body": {"Root": {"Class": "/Script/Engine.CameraActor"}}})json")},
		{TEXT("CameraAnimationSequence"), TEXT(R"json({"SchemaVersion": 1, "Class": "/Script/TemplateSequence.CameraAnimationSequence", "Action": "Create", "Target": "%TARGET%",
			"Properties": {}, "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 30}}, "Root": {"Class": "/Script/Engine.CameraActor",
				"Tracks": [{"Type": "Transform", "Sections": [{"Channels": {"Location.X": {"Keys": [{"Frame": 0, "Value": 0, "Interp": "Linear"},
					{"Frame": 30, "Value": 100, "Interp": "Linear"}]}}}]}],
				"Children": [{"Name": "CameraComponent", "Class": "/Script/Engine.CameraComponent", "Tracks": [{"Type": "Float", "Property": "FieldOfView",
					"Sections": [{"Channels": {"Value": {"Keys": [{"Frame": 0, "Value": 90, "Interp": "Linear"}, {"Frame": 30, "Value": 60, "Interp": "Linear"}]}}}]}]}]}}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRevisionReloadAnimationTest, "UEAssetDoc.RevisionReload.Animation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRevisionReloadAnimationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRevisionReloadTestsLocal;
	static const FCase SlotSkeleton = {TEXT("Skeleton copy with a slot (montage skeleton)"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%",
		"Class": "/Script/Engine.Skeleton", "Action": "Update", "Properties": {"CompatibleSkeletons": ["/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton"]},
		"Body": {"SlotGroups": [{"Name": "DefaultGroup", "Slots": ["DefaultSlot", "UpperBody"]}]}})json"),
		TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton")};
	const FCase Cases[] = {
		{TEXT("Skeleton"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.Skeleton", "Action": "Update", "Body": {
				"SlotGroups": [{"Name": "DefaultGroup", "Slots": ["DefaultSlot", "UpperBody"]}, {"Name": "Face", "Slots": ["Jaw"]}],
				"Sockets": [{"Name": "hand_r_weapon", "Bone": "hand_r", "Location": {"X": 0, "Y": 5, "Z": 0}, "Rotation": {"Pitch": 0, "Yaw": 90, "Roll": 0}}],
				"VirtualBones": [{"Name": "VB hand_ik", "Source": "pelvis", "Target": "hand_r"}],
				"BlendProfiles": [{"Name": "UpperBodyBlend", "Mode": "WeightFactor", "Bones": {"spine_01": 1.5}}],
				"BoneRetargeting": {"pelvis": "AnimationScaled"}, "Curves": [{"Name": "CTRL_Blink", "Material": false, "MorphTarget": true}]}})json"), TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton")},
		{TEXT("AnimSequence"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.AnimSequence", "Action": "Update", "Properties": {},
			"Body": {"Notifies": [{"Time": 0.25, "Name": "Step"}], "Curves": [{"Name": "Weight", "Keys": [{"Time": 0, "Value": 0, "InterpMode": "Linear"},
				{"Time": 0.5, "Value": 1, "InterpMode": "Linear"}]}]}})json"), TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle")},
		{TEXT("AnimMontage"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.AnimMontage", "Action": "Create", "Definitions": {}, "Properties": {},
			"Body": {"Skeleton": "%DEP%", "SlotAnimTracks": [{"SlotName": "UpperBody", "Segments": [{"AnimReference": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle",
				"StartPos": 0.0, "AnimStartTime": 0.0, "AnimEndTime": 1.0, "AnimPlayRate": 1.0, "LoopingCount": 1}]}],
				"CompositeSections": [{"Name": "Default", "NextSectionName": null, "LinkableTime": 0.0, "SlotName": "UpperBody", "LinkMethod": 0}]}})json"), nullptr, &SlotSkeleton},
		{TEXT("AnimComposite"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.AnimComposite", "Action": "Create", "Properties": {},
			"Body": {"Skeleton": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton", "Segments": [{"Animation": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle"},
				{"Animation": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd", "PlayRate": 1.5, "LoopCount": 2}]}})json")},
		{TEXT("BlendSpace"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.BlendSpace", "Action": "Create", "Properties": {},
			"Body": {"Skeleton": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton", "PreviewMesh": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
				"Axes": {"X": {"Name": "Direction", "Min": -180, "Max": 180, "GridDivisions": 4, "SnapToGrid": true}, "Y": {"Name": "Speed", "Min": 0, "Max": 600, "GridDivisions": 4}},
				"Samples": [{"Animation": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle", "X": 0, "Y": 0}, {"Animation": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd", "X": 0, "Y": 300, "RateScale": 1.25}]}})json")},
		{TEXT("AimOffsetBlendSpace"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.AimOffsetBlendSpace", "Action": "Create", "Properties": {},
			"Body": {"Skeleton": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton", "Axes": {"X": {"Name": "Yaw", "Min": -90, "Max": 90}, "Y": {"Name": "Pitch", "Min": -45, "Max": 45}},
				"Samples": []}})json")},
		{TEXT("PoseAsset"), TEXT(R"json({"SchemaVersion": 1, "Target": "%TARGET%", "Class": "/Script/Engine.PoseAsset", "Action": "Create", "Properties": {},
			"Body": {"SourceAnimation": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd", "PreviewMesh": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP"}})json")},
	};
	RunCases(*this, Cases);
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
