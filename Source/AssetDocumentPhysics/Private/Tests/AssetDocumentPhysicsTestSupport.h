// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Shared fixtures of the Physics Asset automation tests: /Game targets, the corpus (design §6.1: the engine's tutorial
// mannequin, the template packs copied into /Game as creating a project from a template does, optional plugin
// content), assets generated as the editor's "Create Physics Asset" does after its dialog, the Details panel's
// property edit, Service calls and comparisons.

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
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentPhysicsTests
{
const TCHAR* const FolderName = TEXT("AssetDocumentPhysics");
const TCHAR* const PhysicsAssetClass = TEXT("/Script/Engine.PhysicsAsset");
// Always mounted (Engine content): the UE4 mannequin of the editor tutorials and its physics asset.
const TCHAR* const TutorialMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
const TCHAR* const TutorialPhysicsAsset = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_PhysicsAsset.TutorialTPP_PhysicsAsset");

inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentPhysicsTests", "Cleanup", "AssetDocument Physics test cleanup"));
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
	UPhysicsAsset* GetAsset() const { return Cast<UPhysicsAsset>(Get()); }

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

// Test assets, removed in reverse order.
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

// Shared content packs of the engine's templates (Templates/TemplateResources/<Level>/<Mount>/Content) copied into
// /Game/<Mount> as creating a project from a template does (the pack is copied whole, so every reference of its
// meshes resolves); only what this test copied is removed again.
struct FTemplatePacks
{
	TArray<FString> CopiedFiles;
	TArray<FString> CopiedPackages;
	TArray<FString> CreatedDirectories;

	explicit FTemplatePacks(std::initializer_list<TPair<const TCHAR*, const TCHAR*>> Packs)
	{
		for (const TPair<const TCHAR*, const TCHAR*>& Pack : Packs)
		{
			const FString SourceDir = FPaths::Combine(FPaths::RootDir(), TEXT("Templates/TemplateResources"), Pack.Key, Pack.Value, TEXT("Content"));
			const FString TargetDir = FPaths::Combine(FPaths::ProjectContentDir(), Pack.Value);
			if (!IFileManager::Get().DirectoryExists(*SourceDir) || IFileManager::Get().DirectoryExists(*TargetDir))
			{
				continue; // not installed, or the project has the pack already (left alone)
			}
			CreatedDirectories.Add(TargetDir);
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*.uasset"), true, false);
			for (const FString& From : Files)
			{
				FString Relative = From;
				FPaths::MakePathRelativeTo(Relative, *(SourceDir + TEXT("/")));
				const FString To = FPaths::Combine(TargetDir, Relative);
				if (IFileManager::Get().Copy(*To, *From) == COPY_OK)
				{
					CopiedFiles.Add(To);
					CopiedPackages.Add(FString(TEXT("/Game/")) + Pack.Value + TEXT("/") + FPaths::GetBaseFilename(Relative, false));
				}
			}
		}
		if (!CopiedFiles.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanFilesSynchronous(CopiedFiles, true);
		}
	}

	FTemplatePacks(const FTemplatePacks&) = delete;
	FTemplatePacks& operator=(const FTemplatePacks&) = delete;

	~FTemplatePacks()
	{
		if (CopiedFiles.IsEmpty())
		{
			return;
		}
		ReleaseUndoHistory();
		TArray<UPackage*> Loaded;
		for (const FString& Package : CopiedPackages)
		{
			if (UPackage* Found = FindPackage(nullptr, *Package))
			{
				Found->ClearDirtyFlag();
				Loaded.Add(Found);
			}
		}
		if (!Loaded.IsEmpty())
		{
			UPackageTools::UnloadPackages(Loaded);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		for (const FString& File : CopiedFiles)
		{
			IFileManager::Get().Delete(*File, false, true);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(CopiedFiles);
		for (const FString& Dir : CreatedDirectories)
		{
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
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

inline FString SerializePretty(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<>::Create(&Text);
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

// Duplicates Source into Target's path (the Content Browser's Duplicate) and saves it.
inline UPhysicsAsset* Duplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Copy = AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), Source);
	if (Copy == nullptr)
	{
		return nullptr;
	}
	Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Target.AssetName), Save(*Copy));
	return Target.GetAsset();
}

// The editor's "Create > Physics Asset" on a skeletal mesh after its generation dialog
// (UPhysicsAssetFactory::CreatePhysicsAssetFromMesh): a new asset, FPhysicsAssetUtils::CreateFromSkeletalMesh with the
// dialog's settings, not assigned to the mesh. Saved.
inline UPhysicsAsset* Generate(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* MeshPath, const FPhysAssetCreateParams& Params)
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, MeshPath);
	if (!Test.TestNotNull(*FString::Printf(TEXT("mesh %s loads"), MeshPath), Mesh))
	{
		return nullptr;
	}
	UPackage* Package = CreatePackage(*Target.PackageName);
	UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(Package, *Target.AssetName, RF_Public | RF_Standalone | RF_Transactional);
	FText Error;
	if (!Test.TestTrue(TEXT("CreateFromSkeletalMesh"), FPhysicsAssetUtils::CreateFromSkeletalMesh(Asset, Mesh, Params, Error, false, false)))
	{
		Test.AddInfo(Error.ToString());
		return nullptr;
	}
	Asset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Asset);
	Test.TestTrue(TEXT("generated asset saved"), Save(*Asset));
	return Target.GetAsset();
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

inline TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action = TEXT("Update"))
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

inline FString FirstDifference(const FString& A, const FString& B)
{
	int32 Index = 0;
	while (Index < A.Len() && Index < B.Len() && A[Index] == B[Index])
	{
		++Index;
	}
	const int32 Start = FMath::Max(0, Index - 120);
	return FString::Printf(TEXT("at %d: '%s' vs '%s'"), Index, *A.Mid(Start, 260), *B.Mid(Start, 260));
}

inline double Milliseconds(double Start)
{
	return (FPlatformTime::Seconds() - Start) * 1000.0;
}

// Unloads the saved package of Target and loads it again from disk.
inline UPhysicsAsset* Reload(const FTarget& Target)
{
	if (UObject* Asset = Target.Get())
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Asset->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	return LoadObject<UPhysicsAsset>(nullptr, *Target.ObjectPath);
}

inline TSharedPtr<FJsonObject> BodyOf(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	return Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) ? *Body : MakeShared<FJsonObject>();
}

inline TSharedPtr<FJsonObject> PropertiesOf(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (Document.IsValid() && Document->TryGetObjectField(TEXT("Properties"), Properties))
	{
		return *Properties;
	}
	const TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
	if (Document.IsValid())
	{
		Document->SetObjectField(TEXT("Properties"), Created);
	}
	return Created;
}

inline int32 CountField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Map = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (!Object.IsValid())
	{
		return 0;
	}
	if (Object->TryGetObjectField(Field, Map))
	{
		return (*Map)->Values.Num();
	}
	return Object->TryGetArrayField(Field, List) ? List->Num() : 0;
}

inline FString StatusOf(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Serialize(Extracted.Compile) : FString(TEXT("-"));
}

inline FString StatusName(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Extracted.Compile->GetStringField(TEXT("Status")) : FString(TEXT("-"));
}

// The Details panel's edit of one (nested) property: the edit chain from the object's member down to the leaf
// ("Member.Inner.Leaf"; a number after an array member picks the element), PreEditChange on the chain, the typed text
// written as the property row imports it, PostEditChangeChainProperty.
inline bool DetailsEdit(FAutomationTestBase& Test, UObject& Owner, const FString& Path, const TCHAR* Text)
{
	TArray<FString> Parts;
	Path.ParseIntoArray(Parts, TEXT("."));
	FEditPropertyChain Chain;
	const UStruct* Struct = Owner.GetClass();
	void* Container = &Owner;
	FProperty* Leaf = nullptr;
	void* LeafValue = nullptr;
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		FProperty* Property = Struct != nullptr ? Struct->FindPropertyByName(FName(*Parts[Index])) : nullptr;
		if (!Test.TestNotNull(*FString::Printf(TEXT("property %s of %s"), *Parts[Index], *Path), Property))
		{
			return false;
		}
		Chain.AddTail(Property);
		void* Value = Property->ContainerPtrToValuePtr<void>(Container);
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property); Array != nullptr && Index + 1 < Parts.Num() && Parts[Index + 1].IsNumeric())
		{
			FScriptArrayHelper Helper(Array, Value);
			const int32 Element = FCString::Atoi(*Parts[Index + 1]);
			if (!Test.TestTrue(*FString::Printf(TEXT("%s: element %d"), *Path, Element), Helper.IsValidIndex(Element)))
			{
				return false;
			}
			Value = Helper.GetRawPtr(Element);
			Parts.RemoveAt(Index + 1);
			const FStructProperty* Inner = CastField<FStructProperty>(Array->Inner);
			Struct = Inner != nullptr ? Inner->Struct : nullptr;
			Container = Value;
			Leaf = Array->Inner;
			LeafValue = Value;
			continue;
		}
		Leaf = Property;
		LeafValue = Value;
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		Struct = StructProperty != nullptr ? StructProperty->Struct : nullptr;
		Container = Value;
	}
	Chain.SetActiveMemberPropertyNode(Chain.GetHead()->GetValue());
	Chain.SetActivePropertyNode(Chain.GetTail()->GetValue());
	Owner.Modify();
	Owner.PreEditChange(Chain);
	const bool bImported = Leaf != nullptr && Leaf->ImportText_Direct(Text, LeafValue, &Owner, PPF_None) != nullptr;
	Test.TestTrue(*FString::Printf(TEXT("%s = %s"), *Path, Text), bImported);
	FPropertyChangedEvent Event(Chain.GetTail()->GetValue(), EPropertyChangeType::ValueSet);
	FPropertyChangedChainEvent ChainEvent(Chain, Event);
	Owner.PostEditChangeChainProperty(ChainEvent);
	return bImported;
}

struct FCorpusEntry
{
	const TCHAR* Path;
	const TCHAR* Source;
	const TCHAR* Why;
};

// Design §6.1. Template packs: copied in by FTemplatePacks (CorpusPacks); plugins: used when enabled (the test sandbox
// enables MoverExamples; ChaosModularVehicleExamples / MetaHumanCharacter are optional).
const FCorpusEntry Corpus[] = {
	{TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_PhysicsAsset.TutorialTPP_PhysicsAsset"), TEXT("Engine"),
		TEXT("UE4 mannequin ragdoll, capsules + spheres, saved long ago")},
	{TEXT("/Game/Characters/Mannequins/Rigs/PA_Mannequin.PA_Mannequin"), TEXT("Template:Characters"), TEXT("UE5 Manny, current format")},
	{TEXT("/Game/Weapons/Rifle/Meshes/PA_Rifle.PA_Rifle"), TEXT("Template:Weapons"), TEXT("prop with boxes")},
	{TEXT("/Game/Weapons/Pistol/Meshes/PA_Pistol.PA_Pistol"), TEXT("Template:Weapons"), TEXT("small prop")},
	{TEXT("/Game/Weapons/GrenadeLauncher/Meshes/PA_GrenadeLauncher.PA_GrenadeLauncher"), TEXT("Template:Weapons"), TEXT("prop")},
	{TEXT("/Game/Vehicles/SportsCar/PA_SportsCar.PA_SportsCar"), TEXT("Template:Vehicles"), TEXT("vehicle: convex chassis + wheel spheres")},
	{TEXT("/Game/Vehicles/OffroadCar/SKM_Offroad_PhysicsAsset.SKM_Offroad_PhysicsAsset"), TEXT("Template:Vehicles"), TEXT("vehicle with suspension bodies")},
	{TEXT("/MoverExamples/Characters/Mannequins/Rigs/PA_Mannequin.PA_Mannequin"), TEXT("MoverExamples"), TEXT("plugin copy of Manny")},
	{TEXT("/ChaosModularVehicleExamples/Models/SportsCar/SportsCar_PhysicsAsset.SportsCar_PhysicsAsset"), TEXT("ChaosModularVehicleExamples"),
		TEXT("modular vehicle")},
	{TEXT("/MetaHumanCharacter/Face/ARKit/PA_MetaHuman_ARKit_Mapping.PA_MetaHuman_ARKit_Mapping"), TEXT("MetaHumanCharacter"), TEXT("MetaHuman")},
};

inline FTemplatePacks CorpusPacks()
{
	return FTemplatePacks({{TEXT("High"), TEXT("Characters")}, {TEXT("Standard"), TEXT("Weapons")}, {TEXT("Standard"), TEXT("Vehicles")}});
}

// The corpus asset when its content is there (a plugin that is not enabled has no mount point: no load is tried).
inline UPhysicsAsset* LoadCorpus(const FCorpusEntry& Entry)
{
	const FString Package = FPackageName::ObjectPathToPackageName(FString(Entry.Path));
	if (!FPackageName::IsValidLongPackageName(Package) || !FPackageName::DoesPackageExist(Package))
	{
		return nullptr;
	}
	return Cast<UPhysicsAsset>(LoadQuiet(Entry.Path));
}

inline FString CorpusName(const FCorpusEntry& Entry)
{
	return FString(Entry.Source) + TEXT(":") + FPackageName::ObjectPathToObjectName(FString(Entry.Path));
}
} // namespace AssetDocumentPhysicsTests
#endif
