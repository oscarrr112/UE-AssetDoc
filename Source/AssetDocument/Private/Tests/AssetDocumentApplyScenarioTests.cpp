// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Data-driven ApplyFile/Extract scenarios through the production Service.
// Each TestData/ApplyScenarios/<Name>.json becomes one automation test
// "UEAssetDoc.GenericRuntime.ApplyScenario.<Name>", so a
// Python-only Profile/component change can carry its own production evidence
// without any C++ change. Scenario shape:
//
// {
//   "Class": "/Script/Engine.CurveFloat",
//   "Steps": [
//     { "Name": "Create", "Action": "Create", "Properties": { ... },
//       "ExpectProperties": { ... },   // optional; default: the authored Properties
//       "ExpectFailure": "Code",       // optional; the apply must fail with this code
//       "ReapplyExtract": true }       // optional; the extract re-applied is a no-op
//   ]
// }
//
// Success steps compare each expected property with the fresh extract. Failure
// steps prove no leftovers (nothing created yet) or an unchanged asset (package
// bytes, clean package and extract).

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
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
namespace AssetDocumentApplyScenarioTests
{
FString ScenarioDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAssetDoc"));
	return Plugin.IsValid()
		? FPaths::Combine(Plugin->GetBaseDir(), TEXT("TestData"), TEXT("ApplyScenarios"))
		: FString();
}

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const FString& Scenario)
		: AssetName(FString::Printf(TEXT("%s_%s"), *Scenario, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/AssetDocumentApplyScenario/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentApplyScenario"),
			AssetName + TEXT(".assetdoc.json"));
	}

	void Cleanup() const
	{
		UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			if (Asset != nullptr)
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		else if (Asset != nullptr)
		{
			ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

FString Serialize(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	return Text;
}

FString Serialize(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return TEXT("<missing>");
	}
	const TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
	Wrapper->SetField(TEXT("v"), Value);
	return Serialize(Wrapper);
}

void Report(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const FString& Stage)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult ApplyText(FAutomationTestBase& Test, const FTarget& Target, const FString& Text, const FString& Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	Test.TestTrue(*FString::Printf(TEXT("%s writes its sidecar"), *Stage),
		FFileHelper::SaveStringToFile(Text, *Target.SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Result, Stage);
	return Result;
}

TSharedPtr<FJsonObject> ExtractProperties(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage,
	TSharedPtr<FJsonObject>* OutDocument = nullptr)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	// Every writable value, so an authored value equal to its class default is listed.
	Request.bDiffOnly = false;
	const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(Request);
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s extracts"), *Stage), Extracted.IsSuccess() && Extracted.Payload.IsValid())
		|| !Extracted.Payload->TryGetObjectField(TEXT("Properties"), Properties) || Properties == nullptr)
	{
		Report(Test, Extracted, Stage + TEXT(" extract"));
		return nullptr;
	}
	if (OutDocument != nullptr)
	{
		*OutDocument = Extracted.Payload;
	}
	return *Properties;
}

TArray<uint8> PackageBytes(const FTarget& Target)
{
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Target.PackageFile);
	return Bytes;
}

bool HasCode(const FAssetDocumentResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

void ExpectProperties(FAutomationTestBase& Test, const FString& Stage, const FJsonObject& Expected, const FJsonObject& Extracted)
{
	for (const auto& Pair : Expected.Values)
	{
		const TSharedPtr<FJsonValue> Actual = Extracted.TryGetField(Pair.Key);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract equals authored %s"), *Stage, *Pair.Key),
				Actual.IsValid() && FJsonValue::CompareEqual(*Actual, *Pair.Value)))
		{
			Test.AddInfo(FString::Printf(TEXT("%s: %s expected %s actual %s"), *Stage, *Pair.Key,
				*Serialize(Pair.Value), *Serialize(Actual)));
		}
	}
}

void ReapplyExtract(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage,
	const TSharedRef<FJsonObject>& Document, const FJsonObject& Properties)
{
	const TArray<uint8> Before = PackageBytes(Target);
	const FString ReapplyStage = Stage + TEXT(" reapply extract");
	const FAssetDocumentResult Result = ApplyText(Test, Target, Serialize(Document), ReapplyStage);
	Test.TestTrue(*FString::Printf(TEXT("%s succeeds"), *ReapplyStage), Result.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s saves nothing"), *ReapplyStage), Result.bSavedAsset);
	Test.TestTrue(*FString::Printf(TEXT("%s leaves the package bytes"), *ReapplyStage), PackageBytes(Target) == Before);
	const UPackage* Package = FindPackage(nullptr, *Target.PackageName);
	Test.TestTrue(*FString::Printf(TEXT("%s leaves the package clean"), *ReapplyStage), Package != nullptr && !Package->IsDirty());
	if (const TSharedPtr<FJsonObject> After = ExtractProperties(Test, Target, ReapplyStage))
	{
		ExpectProperties(Test, ReapplyStage, Properties, *After);
	}
}

void RunScenario(FAutomationTestBase& Test, const FString& Name, const FJsonObject& Scenario)
{
	FString Class;
	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!Test.TestTrue(TEXT("scenario declares Class and Steps"),
			Scenario.TryGetStringField(TEXT("Class"), Class) && Scenario.TryGetArrayField(TEXT("Steps"), Steps)
			&& Steps != nullptr && !Steps->IsEmpty()))
	{
		return;
	}
	const FTarget Target(Name);
	ON_SCOPE_EXIT { Target.Cleanup(); };
	bool bExists = false;
	for (int32 Index = 0; Index < Steps->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* Step = nullptr;
		FString Action;
		const TSharedPtr<FJsonObject>* Authored = nullptr;
		if (!Test.TestTrue(*FString::Printf(TEXT("step %d declares Action and Properties"), Index),
				(*Steps)[Index]->TryGetObject(Step) && Step != nullptr
				&& (*Step)->TryGetStringField(TEXT("Action"), Action)
				&& (*Step)->TryGetObjectField(TEXT("Properties"), Authored) && Authored != nullptr))
		{
			return;
		}
		FString StepName = FString::Printf(TEXT("step %d"), Index);
		(*Step)->TryGetStringField(TEXT("Name"), StepName);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetNumberField(TEXT("SchemaVersion"), 1);
		Document->SetStringField(TEXT("AssetType"), TEXT("GenericAsset"));
		Document->SetStringField(TEXT("Target"), Target.PackageName);
		Document->SetStringField(TEXT("Class"), Class);
		Document->SetStringField(TEXT("Action"), Action);
		Document->SetObjectField(TEXT("Properties"), *Authored);

		FString ExpectedFailure;
		const bool bExpectFailure = (*Step)->TryGetStringField(TEXT("ExpectFailure"), ExpectedFailure);
		const TArray<uint8> BytesBefore = bExists ? PackageBytes(Target) : TArray<uint8>();
		const TSharedPtr<FJsonObject> PropertiesBefore = bExists ? ExtractProperties(Test, Target, StepName + TEXT(" before")) : nullptr;

		const FAssetDocumentResult Result = ApplyText(Test, Target, Serialize(Document), StepName);
		if (bExpectFailure)
		{
			Test.TestFalse(*FString::Printf(TEXT("%s fails"), *StepName), Result.IsSuccess());
			Test.TestTrue(*FString::Printf(TEXT("%s reports %s"), *StepName, *ExpectedFailure), HasCode(Result, ExpectedFailure));
			if (!bExists)
			{
				Test.TestNull(*FString::Printf(TEXT("%s leaves no package"), *StepName),
					FindPackage(nullptr, *Target.PackageName));
				Test.TestFalse(*FString::Printf(TEXT("%s leaves no package file"), *StepName),
					IFileManager::Get().FileExists(*Target.PackageFile));
				continue;
			}
			Test.TestTrue(*FString::Printf(TEXT("%s leaves the package bytes"), *StepName), PackageBytes(Target) == BytesBefore);
			const UPackage* Package = FindPackage(nullptr, *Target.PackageName);
			Test.TestTrue(*FString::Printf(TEXT("%s leaves the package clean"), *StepName), Package != nullptr && !Package->IsDirty());
			const TSharedPtr<FJsonObject> After = ExtractProperties(Test, Target, StepName + TEXT(" after"));
			Test.TestTrue(*FString::Printf(TEXT("%s leaves the extract"), *StepName), PropertiesBefore.IsValid() && After.IsValid()
				&& FJsonValue::CompareEqual(FJsonValueObject(After), FJsonValueObject(PropertiesBefore)));
			continue;
		}
		if (!Test.TestTrue(*FString::Printf(TEXT("%s succeeds"), *StepName), Result.IsSuccess()))
		{
			return;
		}
		bExists = true;
		TSharedPtr<FJsonObject> ExtractedDocument;
		const TSharedPtr<FJsonObject> Extracted = ExtractProperties(Test, Target, StepName, &ExtractedDocument);
		if (!Extracted.IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonObject>* Expected = nullptr;
		ExpectProperties(Test, StepName,
			(*Step)->TryGetObjectField(TEXT("ExpectProperties"), Expected) && Expected != nullptr ? **Expected : **Authored,
			*Extracted);
		bool bReapply = false;
		if ((*Step)->TryGetBoolField(TEXT("ReapplyExtract"), bReapply) && bReapply)
		{
			ReapplyExtract(Test, Target, StepName, ExtractedDocument.ToSharedRef(), *Extracted);
		}
	}
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FAssetDocumentApplyScenarioTest,
	"UEAssetDoc.GenericRuntime.ApplyScenario",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FAssetDocumentApplyScenarioTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	const FString Directory = AssetDocumentApplyScenarioTests::ScenarioDirectory();
	if (Directory.IsEmpty())
	{
		return;
	}
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
	Files.Sort();
	for (const FString& File : Files)
	{
		OutBeautifiedNames.Add(FPaths::GetBaseFilename(File));
		OutTestCommands.Add(FPaths::Combine(Directory, File));
	}
}

bool FAssetDocumentApplyScenarioTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentApplyScenarioTests;
	FString Text;
	// Number literals stay verbatim, so an authored 2.0 is written back as a float.
	TSharedPtr<FJsonObject> Scenario;
	if (!TestTrue(TEXT("scenario file loads"), FFileHelper::LoadFileToString(Text, *Parameters))
		|| !TestTrue(TEXT("scenario file is a JSON object"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Scenario,
				FJsonSerializer::EFlags::StoreNumbersAsStrings) && Scenario.IsValid()))
	{
		return false;
	}
	RunScenario(*this, FPaths::GetBaseFilename(Parameters), *Scenario);
	return !HasAnyErrors();
}
#endif
