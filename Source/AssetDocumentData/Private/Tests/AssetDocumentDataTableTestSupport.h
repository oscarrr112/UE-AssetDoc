// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Shared harness of the data table family end-to-end tests: documents go through the
// production Service (ApplyFile / Extract / Diff / Inspect) exactly as the MCP tools send them.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintActionDatabase.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentDataTableTests
{
inline const TCHAR* FolderName = TEXT("AssetDocumentDataTablePython");

// Editor operations open transactions; clear the undo buffer before force-deleting fixtures.
inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentDataTableTests", "Cleanup", "AssetDocument data table test cleanup"));
	}
}

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

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	template <typename T>
	T* Get() const { return Cast<T>(Get()); }

	bool Exists() const
	{
		return Get() != nullptr || IFileManager::Get().FileExists(*PackageFile)
			|| FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	// Saved tables are unloaded and unsaved ones deleted as the Content Browser does. A loaded
	// string table is always referenced in memory (FStringTableEngineBridge keeps it alive until it
	// is garbage), so it needs the delete dialog's Force Delete, as in the editor.
	void Cleanup() const
	{
		ReleaseUndoHistory();
		bool bForceDeleted = false;
		if (UObject* Asset = Get())
		{
			if (Asset->IsA<UStringTable>())
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				bForceDeleted = ObjectTools::ForceDeleteObjects({Asset}, false) > 0;
			}
			else if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			if (!bForceDeleted)
			{
				// (Force Delete already removed the registry entry.)
				FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
			}
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

inline void LogResult(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

// Validate of an inline document (no sidecar written).
inline FAssetDocumentResult ValidateDocument(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentValidateRequest Request;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Request.Document);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

inline FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

inline TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

inline FString Inspect(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Inspect(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: inspect succeeds"), Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return FString();
	}
	return Serialize(Result.Payload);
}

inline void TestNoDiff(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const bool bOk = Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), Stage), bOk))
	{
		LogResult(Test, Stage, Result);
		return;
	}
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: live asset equals the document"), Stage), Changed->Num(), 0))
	{
		Test.AddInfo(Serialize(Result.Payload));
	}
}

inline bool ExtractEquals(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected, const TCHAR* Stage)
{
	const FString Actual = Serialize(Extract(Test, Target, Stage));
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: extraction is unchanged"), Stage), Actual, Expected))
	{
		Test.AddInfo(FString::Printf(TEXT("expected %s"), *Expected));
		Test.AddInfo(FString::Printf(TEXT("actual   %s"), *Actual));
		return false;
	}
	return true;
}

// The failure carries Code at Path (an empty Path accepts any path).
inline bool TestFailsWith(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* Path, const TCHAR* Stage)
{
	Test.TestFalse(*FString::Printf(TEXT("%s: fails"), Stage), Result.IsSuccess());
	const bool bFound = Result.Diagnostics.ContainsByPredicate([Code, Path](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && (*Path == TEXT('\0') || Diagnostic.Path == Path);
	});
	return Test.TestTrue(*FString::Printf(TEXT("%s: diagnostic %s at %s"), Stage, Code, *Path ? Path : TEXT("(any)")), bFound);
}

// Re-applying the extraction (Action Update) saves nothing and leaves the package file alone;
// returns the extraction as CreateOrUpdate (the Extract baseline).
inline FString TestNoOpReapply(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Target, Stage);
	UObject* Asset = Target.Get();
	if (!Extracted.IsValid() || Asset == nullptr)
	{
		return FString();
	}
	const FString Baseline = Serialize(Extracted);
	Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Update = Serialize(Extracted);
	Test.AddInfo(FString::Printf(TEXT("%s: extracted %s"), Stage, *Update));
	Test.TestFalse(*FString::Printf(TEXT("%s: package is saved clean"), Stage), Asset->GetOutermost()->IsDirty());
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	const FAssetDocumentResult Reapplied = Apply(Test, Target, Update, Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: extraction re-applies"), Stage), Reapplied.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: no-op re-apply reports no save"), Stage), Reapplied.bSavedAsset);
	Test.TestFalse(*FString::Printf(TEXT("%s: no-op re-apply leaves the package clean"), Stage), Asset->GetOutermost()->IsDirty());
	Test.TestEqual(*FString::Printf(TEXT("%s: no-op re-apply does not rewrite the package file"), Stage),
		IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
	Test.TestTrue(*FString::Printf(TEXT("%s: same asset object"), Stage), Target.Get() == Asset);
	ExtractEquals(Test, Target, Baseline, Stage);
	TestNoDiff(Test, Update, Stage);
	return Baseline;
}

// The saved package, unloaded and loaded back from disk, extracts to Expected.
inline void TestReload(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected)
{
	UObject* Asset = Target.Get();
	if (Asset == nullptr)
	{
		return;
	}
	Test.TestFalse(TEXT("package is clean before unload"), Asset->GetOutermost()->IsDirty());
	ReleaseUndoHistory();
	// Once an earlier test in the process created the Blueprint action database (any Blueprint
	// work does), it holds node spawners (Make/Break) for every User Defined Struct or Enum, and
	// the editor's unload releases them only for Blueprints: drop this asset's entries first.
	if (FBlueprintActionDatabase* Actions = FBlueprintActionDatabase::TryGet())
	{
		Actions->ClearAssetActions(Asset);
	}
	UPackageTools::UnloadPackages({Asset->GetOutermost()});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Test.TestNull(TEXT("table is unloaded"), Target.Get());
	ExtractEquals(Test, Target, Expected, TEXT("after reload from disk"));
}

// A failed create leaves no asset, package file or registry entry.
inline void TestNothingCreated(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	Test.TestNull(*FString::Printf(TEXT("%s: no asset remains"), Stage), Target.Get());
	Test.TestFalse(*FString::Printf(TEXT("%s: no package file"), Stage), IFileManager::Get().FileExists(*Target.PackageFile));
	Test.TestFalse(*FString::Printf(TEXT("%s: no registry entry"), Stage),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid());
}

inline FString Document(const FTarget& Target, const TCHAR* Class, const TCHAR* Action, const FString& Body, const FString& Properties = TEXT("{}"))
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Properties\":%s,\"Body\":{%s}}"),
		*Target.PackageName, Class, Action, *Properties, *Body);
}

inline TArray<TSharedPtr<FJsonValue>> BodyArray(const TSharedPtr<FJsonObject>& Document, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetArrayField(Field, Values))
	{
		return *Values;
	}
	return {};
}
} // namespace AssetDocumentDataTableTests
#endif
