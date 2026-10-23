// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Shared harness of the Niagara end-to-end tests. Corpus assets live under plugin mounts (/Niagara/...),
// which the Profiles do not extract yet, so each one is duplicated to /Game/AssetDocumentNiagaraTests
// (as a user would with the Content Browser's Duplicate) and the duplicate goes through the production
// Service (Extract / Inspect / ApplyFile / Diff) exactly as the MCP tools send documents. Plugin content is
// never modified.

#include "AssetDocumentModule.h"
#include "AssetDocumentNiagaraPrimitives.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetCompilingManager.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IAssetTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraTests
{
inline const TCHAR* FolderName = TEXT("AssetDocumentNiagaraTests");

inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentNiagaraTests", "Cleanup", "AssetDocument Niagara test cleanup"));
	}
}

// HLSL that does not compile (the text names Symbol): engines whose Vector VM compile is a shader compile also log the
// shader compiler's errors as an error of their own.
inline void ExpectVectorVMCompileErrors(FAutomationTestBase& Test, const TCHAR* Symbol)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	Test.AddExpectedError(FString::Printf(TEXT("errors encountered compiling Vector VM shaders:[\\s\\S]*%s"), Symbol),
		EAutomationExpectedErrorFlags::Contains, 0);
#endif
}

// Unloads packages once the asset compilers are done with them: a copied texture (a system's thumbnail) can still be
// building, and unloading asks its compiler to finish it.
inline void UnloadPackages(const TArray<UPackage*>& Packages)
{
	FAssetCompilingManager::Get().ProcessAsyncTasks(false);
	FAssetCompilingManager::Get().FinishAllCompilation();
	for (UPackage* Package : Packages)
	{
		ForEachObjectWithPackage(Package, [](UObject* Object)
		{
			if (UTexture* Texture = Cast<UTexture>(Object))
			{
				Texture->BlockOnAnyAsyncBuild();
				// A texture whose resource was made while its data was still building can keep that stand-in after the build
				// (a duplicated asset's thumbnail): it reads as still compiling, which unloading reports. Its resource is
				// made again from the built data.
				if (Texture->IsCompiling())
				{
					Texture->UpdateResource();
				}
			}
			return true;
		});
	}
	UPackageTools::UnloadPackages(Packages);
}

inline double Milliseconds(double Start)
{
	return (FPlatformTime::Seconds() - Start) * 1000.0;
}

// A /Game asset of the test folder (unique name per run).
struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const FString& Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)))
	{
		PackageName = FString(TEXT("/Game/")) + FolderName + TEXT("/") + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), FolderName, AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	bool Exists() const
	{
		return Get() != nullptr || IFileManager::Get().FileExists(*PackageFile);
	}

	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UnloadPackages({Asset->GetOutermost()});
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
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

inline bool SaveAsset(UObject& Asset, const FString& PackageFile)
{
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.Error = GWarn;
	return UPackage::SavePackage(Asset.GetOutermost(), &Asset, *PackageFile, Args);
}

// Content Browser Duplicate of Source into Target, saved.
inline UObject* DuplicateTo(UObject& Source, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Copy = AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), &Source);
	if (Copy != nullptr && !SaveAsset(*Copy, Target.PackageFile))
	{
		return nullptr;
	}
	return Copy;
}

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

inline void LogResult(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	// Also in the editor log: a crash later in the test still leaves the apply's outcome readable.
	UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests %s: %s"), *Stage, *Result.Message);
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests %s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message.Left(2000));
	}
}

inline FAssetDocumentResult ExtractResult(const FString& ObjectPath)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	return FAssetDocumentModule::Get().GetService().Extract(Request);
}

inline TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage, double* OutMs = nullptr)
{
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Result = ExtractResult(Target.ObjectPath);
	if (OutMs != nullptr)
	{
		*OutMs = Milliseconds(Start);
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

inline TSharedPtr<FJsonObject> Inspect(FAutomationTestBase& Test, const FString& ObjectPath, const FString& Stage, double* OutMs = nullptr)
{
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = ObjectPath;
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Inspect(Request);
	if (OutMs != nullptr)
	{
		*OutMs = Milliseconds(Start);
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: inspect succeeds"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

inline FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
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

inline bool HasDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

inline bool TestTextEqual(FAutomationTestBase& Test, const FString& What, const FString& Actual, const FString& Expected)
{
	if (Test.TestTrue(*What, Actual == Expected))
	{
		return true;
	}
	int32 Index = 0;
	while (Index < Actual.Len() && Index < Expected.Len() && Actual[Index] == Expected[Index])
	{
		++Index;
	}
	const int32 From = FMath::Max(0, Index - 200);
	Test.AddInfo(FString::Printf(TEXT("%s: first difference at %d"), *What, Index));
	Test.AddInfo(FString::Printf(TEXT("expected ...%s"), *Expected.Mid(From, 500)));
	Test.AddInfo(FString::Printf(TEXT("actual   ...%s"), *Actual.Mid(From, 500)));
	UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests %s: first difference at %d\n  expected ...%s\n  actual   ...%s"), *What, Index,
		*Expected.Mid(From, 500), *Actual.Mid(From, 500));
	return false;
}
} // namespace AssetDocumentNiagaraTests
#endif
