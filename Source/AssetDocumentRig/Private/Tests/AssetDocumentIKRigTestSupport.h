// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Shared fixtures of the IK Rig / IK Retargeter automation tests: /Game targets, editor-built fixtures (the
// Content Browser's "New IK Rig" on a mesh plus the toolbar's Auto Characterize / Auto FBIK), the corpus
// (design §6.1), Service calls and comparisons.

#pragma once

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "../../../AssetDocument/Private/Tests/AssetDocumentDiffCountTestSupport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetOps.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "Rig/IKRigDefinition.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/ControlRigSettings.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentIKRigTests
{
const TCHAR* const FolderName = TEXT("AssetDocumentIKRig");
const TCHAR* const IKRigClass = TEXT("/Script/IKRig.IKRigDefinition");
const TCHAR* const RetargeterClass = TEXT("/Script/IKRig.IKRetargeter");
// Always mounted (Engine content): the UE4 mannequin of the editor tutorials.
const TCHAR* const TutorialMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");

// The modular rig tests build their rigs from the Modules56 module set. A new modular rig starts with the Control Rig
// settings' default root module, which differs between engines: for the scope it is the Modules56 Root, as a project
// can set it.
struct FModules56RootScope
{
	FSoftObjectPath Previous;
	FModules56RootScope()
	{
		UControlRigSettings* Settings = GetMutableDefault<UControlRigSettings>();
		Previous = Settings->DefaultRootModule;
		Settings->DefaultRootModule = FSoftObjectPath(TEXT("/ControlRig/Modules/Modules56/Root.Root"));
	}
	~FModules56RootScope()
	{
		GetMutableDefault<UControlRigSettings>()->DefaultRootModule = Previous;
	}
	FModules56RootScope(const FModules56RootScope&) = delete;
	FModules56RootScope& operator=(const FModules56RootScope&) = delete;
};

// Control Rig compile errors about stale or unmapped function references. Their wording differs between engines:
// some name the stale pins ("uses [Bones] pins ..."), some also report each variable a reference leaves unmapped.
inline void ExpectStalePinErrors(FAutomationTestBase& Test, int32 Occurrences)
{
	Test.AddExpectedMessage(TEXT("uses (\\[[^\\]]*\\] )?pins that no longer exist"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, Occurrences);
}

inline void ExpectUnmappedVariableErrors(FAutomationTestBase& Test, int32 Occurrences)
{
	Test.AddExpectedMessage(TEXT("has unmapped variables|is not remapped on function reference"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, Occurrences);
}

inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentIKRigTests", "Cleanup", "AssetDocument IK Rig test cleanup"));
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

	// Saved assets are unloaded and deleted from disk as the Content Browser does.
	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
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

// Test assets, removed in reverse order (retargeters before the rigs they reference).
struct FFixtures
{
	TArray<TUniquePtr<FTarget>> Targets;
	const FTarget& Add(const TCHAR* Prefix) { return *Targets.Add_GetRef(MakeUnique<FTarget>(Prefix)); }
	~FFixtures()
	{
		for (int32 Index = Targets.Num() - 1; Index >= 0; --Index)
		{
			Targets[Index]->Cleanup();
		}
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

// Indented JSON with "\n" line ends on every platform (the engine's pretty writer ends lines with the
// platform's terminator, "\r\n" on Windows), so sizes measured from it are the same everywhere.
inline FString SerializePretty(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
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
		Lines.Add(FString::Printf(TEXT("[%s] %s %s%s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message,
			Diagnostic.Details.IsValid() ? *(TEXT(" ") + Serialize(Diagnostic.Details)) : TEXT("")));
	}
	return Lines.IsEmpty() ? Result.Message : FString::Join(Lines, TEXT(" / "));
}

inline FAssetDocumentResult ExtractResult(const FString& ObjectPath)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	return FAssetDocumentModule::Get().GetService().Extract(Request);
}

inline TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FString& ObjectPath, const FString& Stage)
{
	const FAssetDocumentResult Result = ExtractResult(ObjectPath);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return nullptr;
	}
	return Result.Payload;
}

inline FAssetDocumentResult Inspect(const FString& ObjectPath)
{
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = ObjectPath;
	return FAssetDocumentModule::Get().GetService().Inspect(Request);
}

// Number of changes Diff reports between the document and the live asset (changed, added and removed entries; -1 when Diff fails).
inline int32 DiffCount(const TSharedPtr<FJsonObject>& Document, FString& OutText)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Document;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const int32 Count = AssetDocumentDiffCountTestSupport::CountDiffEntries(Result);
	if (Count < 0)
	{
		OutText = DiagnosticsText(Result);
		return -1;
	}
	OutText = Serialize(Result.Payload);
	return Count;
}

inline FAssetDocumentResult Apply(const FTarget& Target, const TSharedPtr<FJsonObject>& Document)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Serialize(Document), *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	return FAssetDocumentModule::Get().GetService().ApplyFile(Request);
}

inline bool HasCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

inline FString FirstPath(const FAssetDocumentResult& Result)
{
	return Result.Diagnostics.IsEmpty() ? FString() : Result.Diagnostics[0].Path;
}

inline UObject* LoadQuiet(const FString& Path)
{
	return LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

inline bool Save(UObject& Asset)
{
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({Asset.GetOutermost()}, false);
	ReleaseUndoHistory();
	return bSaved;
}

// Duplicates Source into Target's path through the duplicate operation (the Content Browser's Duplicate, saved).
inline UObject* Duplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target)
{
	if (Source == nullptr)
	{
		return nullptr;
	}
	FAssetDocumentDuplicateRequest Request;
	Request.Source = Source->GetPathName();
	Request.Target = Target.PackageName;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Duplicate(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s duplicated (%s)"), *Target.AssetName, *DiagnosticsText(Result)), Result.IsSuccess()))
	{
		return nullptr;
	}
	ReleaseUndoHistory();
	return Target.Get();
}

// The Content Browser's Duplicate itself (IAssetTools::DuplicateAsset), saved: the editor-only path.
inline UObject* EditorDuplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Copy = AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), Source);
	if (Copy == nullptr)
	{
		return nullptr;
	}
	Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Target.AssetName), Save(*Copy));
	return Copy;
}

// The Content Browser's "IK Rig" on a skeletal mesh (FAssetTypeActions_IKRigDefinition): the factory, then the mesh;
// bAuto adds the toolbar's Auto Characterize and Auto FBIK (what the batch retarget window does). Saved.
inline UIKRigDefinition* MakeRig(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* MeshPath, bool bAuto)
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, MeshPath);
	if (!Test.TestNotNull(*FString::Printf(TEXT("mesh %s loads"), MeshPath), Mesh))
	{
		return nullptr;
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UIKRigDefinition* Rig = Cast<UIKRigDefinition>(AssetTools.CreateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), UIKRigDefinition::StaticClass(), NewObject<UIKRigDefinitionFactory>()));
	if (!Test.TestNotNull(TEXT("IK Rig created"), Rig))
	{
		return nullptr;
	}
	UIKRigController* Controller = UIKRigController::GetController(Rig);
	Test.TestTrue(TEXT("mesh set"), Controller->SetSkeletalMesh(Mesh));
	if (bAuto)
	{
		Test.TestTrue(TEXT("auto characterize"), Controller->ApplyAutoGeneratedRetargetDefinition());
		Controller->ApplyAutoFBIK();
	}
	Test.TestTrue(TEXT("IK Rig saved"), Save(*Rig));
	return Rig;
}

// The Content Browser's "IK Retargeter" (factory: default ops), then both rigs as the editor's create action sets
// them, and the default ops configured for those rigs. Saved.
inline UIKRetargeter* MakeRetargeter(FAutomationTestBase& Test, const FTarget& Target, UIKRigDefinition* Source, UIKRigDefinition* TargetRig)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(AssetTools.CreateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), UIKRetargeter::StaticClass(), NewObject<UIKRetargetFactory>()));
	if (!Test.TestNotNull(TEXT("IK Retargeter created"), Retargeter))
	{
		return nullptr;
	}
	const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	// Some engines' factory leaves the op stack empty; the op stack's "Add Default Ops" button fills it.
	if (Controller->GetNumRetargetOps() == 0)
	{
		Controller->AddDefaultOps();
	}
	// Some engines' default ops leave Run IK Rig without children; the fixtures have its IK Chains op, as "Add New" adds it.
	UScriptStruct* IKChainsType = FindObject<UScriptStruct>(nullptr, TEXT("/Script/IKRig.IKRetargetIKChainsOp"));
	for (int32 Index = 0; Index < Controller->GetNumRetargetOps() && IKChainsType != nullptr; ++Index)
	{
		const FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(Index);
		if (Op != nullptr && Op->GetType()->GetFName() == TEXT("IKRetargetRunIKRigOp") && Controller->GetChildOpIndices(Index).IsEmpty())
		{
			Controller->AddRetargetOp(IKChainsType, Op->GetName());
			break;
		}
	}
	Controller->SetIKRig(ERetargetSourceOrTarget::Source, Source);
	Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
	Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, Source);
	Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, TargetRig);
	Test.TestTrue(TEXT("IK Retargeter saved"), Save(*Retargeter));
	return Retargeter;
}

inline TSet<FString> DirtyPackages()
{
	TArray<UPackage*> Packages;
	FEditorFileUtils::GetDirtyContentPackages(Packages);
	TSet<FString> Names;
	for (const UPackage* Package : Packages)
	{
		Names.Add(Package->GetName());
	}
	return Names;
}

// Reading leaves no new dirty package.
struct FReadSideEffects
{
	TSet<FString> Dirty = DirtyPackages();

	void Check(TArray<FString>& OutProblems) const
	{
		for (const FString& Name : DirtyPackages())
		{
			if (!Dirty.Contains(Name))
			{
				OutProblems.Add(FString::Printf(TEXT("reading dirtied %s"), *Name));
			}
		}
	}
};

// The document with Target cleared (duplicates differ only there).
inline FString WithoutTarget(const TSharedPtr<FJsonObject>& Document)
{
	if (!Document.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), FString());
	return Serialize(Copy);
}

inline FString FirstDifference(const FString& A, const FString& B)
{
	int32 Index = 0;
	while (Index < A.Len() && Index < B.Len() && A[Index] == B[Index])
	{
		++Index;
	}
	const int32 Start = FMath::Max(0, Index - 80);
	return FString::Printf(TEXT("at %d: '%s' vs '%s'"), Index, *A.Mid(Start, 200), *B.Mid(Start, 200));
}

inline double Milliseconds(double Start)
{
	return (FPlatformTime::Seconds() - Start) * 1000.0;
}

// Unloads the saved package of Target and loads it again from disk.
inline UObject* Reload(const FTarget& Target)
{
	if (UObject* Asset = Target.Get())
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Asset->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	return LoadObject<UObject>(nullptr, *Target.ObjectPath);
}

inline TSharedPtr<FJsonObject> BodyOf(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	return Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) ? *Body : MakeShared<FJsonObject>();
}

inline int32 CountField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Map = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Object->TryGetObjectField(Field, Map))
	{
		return (*Map)->Values.Num();
	}
	return Object->TryGetArrayField(Field, List) ? List->Num() : 0;
}

struct FCorpusEntry
{
	const TCHAR* Path;
	const TCHAR* Plugin;
	const TCHAR* Why;
};

// Design §6.1: the shipped IK content (all in plugins that are off by default; the test sandbox enables
// MoverExamples; MoverTests and MetaHumanCharacter are optional).
const FCorpusEntry Corpus[] = {
	{TEXT("/MetaHumanCharacter/Animation/Retargeting/IK_MH_IKRig.IK_MH_IKRig"), TEXT("MetaHumanCharacter"), TEXT("current format, FBIK goal + bone settings, largest")},
	{TEXT("/MetaHumanCharacter/Animation/Retargeting/RTG_MH_IKRig.RTG_MH_IKRig"), TEXT("MetaHumanCharacter"), TEXT("current op stack")},
	{TEXT("/MoverExamples/Characters/Mannequins/Rigs/IK_Mannequin.IK_Mannequin"), TEXT("MoverExamples"), TEXT("legacy UObject solvers (PostLoad conversion)")},
	{TEXT("/MoverExamples/Characters/Mannequins/Rigs/RTG_Mannequin.RTG_Mannequin"), TEXT("MoverExamples"), TEXT("oldest retargeter format")},
	{TEXT("/MoverTests/Characters/Mannequins/Rigs/IK_Mannequin.IK_Mannequin"), TEXT("MoverTests"), TEXT("same family, saved in Main")},
	{TEXT("/MoverTests/Characters/Mannequins/Rigs/RTG_Mannequin.RTG_Mannequin"), TEXT("MoverTests"), TEXT("same family")},
};
} // namespace AssetDocumentIKRigTests
#endif
