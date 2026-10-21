// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentDuplicate.h"

#include "AssetDocumentAssetCompilation.h"
#include "AssetDocumentDuplicateFixups.h"
#include "Discovery/AssetDocumentDiscovery.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "ExternalPackageHelper.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Selection.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/Linker.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace
{
const TCHAR* GameRoot = TEXT("/Game/");

FString PackageNameOf(const FString& PackageOrObjectPath)
{
	FString Path = PackageOrObjectPath.TrimStartAndEnd();
	FString PackageName;
	FString ObjectName;
	return Path.Split(TEXT("."), &PackageName, &ObjectName) ? PackageName : Path;
}

FAssetDocumentResult MakeDuplicateFailure(
	const FAssetDocumentDuplicateRequest& Request,
	const TCHAR* Path,
	const TCHAR* Code,
	const FString& Message)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	Diagnostics.Add({Path, Code, Message, TEXT("error")});
	FAssetDocumentResult Result = FAssetDocumentResult::FailureFromDiagnostics(Diagnostics, Message);
	Result.Target = PackageNameOf(Request.Target);
	return Result;
}

// The source asset as the Asset Registry lists it, loaded from its package on disk (or the
// loaded in-memory asset). A package path names its asset of the same name, or its only asset.
// Nothing is loaded for a path the registry does not list.
UObject* FindSourceAsset(const FString& Source)
{
	const FString Path = Source.TrimStartAndEnd();
	if (!Path.StartsWith(TEXT("/")) || Path.StartsWith(TEXT("/Script/")))
	{
		return nullptr;
	}
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	const FString PackageName = PackageNameOf(Path);
	const FString ObjectPath = Path.Contains(TEXT("."))
		? Path
		: FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetLongPackageAssetName(Path));
	FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (!AssetData.IsValid() && !Path.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(PackageName, true))
	{
		TArray<FAssetData> PackageAssets;
		Registry.GetAssetsByPackageName(FName(*PackageName), PackageAssets);
		if (PackageAssets.Num() == 1)
		{
			AssetData = PackageAssets[0];
		}
	}
	return AssetData.IsValid() ? AssetData.GetAsset() : nullptr;
}

bool IsDuplicateSourceWorldInUse(const UWorld& World)
{
	if (GEditor == nullptr)
	{
		return false;
	}
	for (const FWorldContext& Context : GEditor->GetWorldContexts())
	{
		if (Context.World() != nullptr && Context.World()->GetPackage() == World.GetPackage())
		{
			return true;
		}
	}
	return false;
}

// Anything under the target name: a registry entry, a package file, or a loaded package that
// holds an object. An empty, clean, never-saved package (what a failed read leaves) is reused.
bool TargetExists(const FString& PackageName)
{
	TArray<FAssetData> Assets;
	FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(FName(*PackageName), Assets, true);
	if (!Assets.IsEmpty() || FPackageName::DoesPackageExist(PackageName))
	{
		return true;
	}
	const UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package == nullptr)
	{
		return false;
	}
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	return Package->IsDirty() || !Objects.IsEmpty();
}

// The editor's discard of an unwanted never-saved package (as the AssetDocument editor provider
// discards a failed create): forget its assets, reset loaders, rename it away and mark it garbage.
// A world in it that the editor initialized (an inactive copy of a map) is cleaned up first, as
// UPackageTools::UnloadPackages does, so its subsystems and World Partition are torn down.
void DiscardDuplicatePackage(UPackage& Package)
{
	AssetDocumentAssetCompilation::FinishPackageCompilation({&Package});
	TArray<UObject*> Objects;
	GetObjectsWithPackage(&Package, Objects);
	for (UObject* Object : Objects)
	{
		if (UWorld* World = Cast<UWorld>(Object); World != nullptr && World->bIsWorldInitialized)
		{
			World->CleanupWorld();
		}
	}
	for (UObject* Object : Objects)
	{
		if (Object->HasAnyFlags(RF_Standalone | RF_Public))
		{
			FAssetRegistryModule::AssetDeleted(Object);
		}
		Object->ClearFlags(RF_Standalone | RF_Public);
		Object->MarkAsGarbage();
	}
	ResetLoaders(&Package);
	Package.SetDirtyFlag(false);
	Package.ClearFlags(RF_Standalone | RF_Public);
	const FName Discarded = MakeUniqueObjectName(
		nullptr, UPackage::StaticClass(), FName(TEXT("/Temp/AssetDocumentDiscardedDuplicate")));
	Package.Rename(*Discarded.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	Package.MarkAsGarbage();
}

void DiscardUnsavedPackage(const FString& PackageName)
{
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package == nullptr || FPackageName::DoesPackageExist(PackageName))
	{
		return;
	}
	DiscardDuplicatePackage(*Package);
}

TOptional<FAssetDocumentResult> RejectTarget(const FAssetDocumentDuplicateRequest& Request, const FString& Target)
{
	FText Reason;
	const FString AssetName = FPackageName::GetLongPackageAssetName(Target);
	if (!Target.StartsWith(GameRoot)
		|| !FPackageName::IsValidLongPackageName(Target, false, &Reason)
		|| AssetName.IsEmpty()
		|| !FName(*AssetName).IsValidObjectName(Reason))
	{
		const FString Why = UE::AssetDocument::IsReadOnlyContentPath(Target)
			? FString(TEXT("it is engine or plugin content, which is read-only"))
			: Reason.IsEmpty() ? FString(TEXT("it is not a /Game package path")) : Reason.ToString();
		return MakeDuplicateFailure(Request, TEXT("/target"), TEXT("Duplicate.InvalidTarget"), FString::Printf(
			TEXT("Target '%s' cannot receive the copy: %s. Use an absent /Game package path such as /Game/Characters/%s."),
			*Target, *Why, AssetName.IsEmpty() ? TEXT("SK_Copy") : *AssetName));
	}
	if (TargetExists(Target))
	{
		return MakeDuplicateFailure(Request, TEXT("/target"), TEXT("Duplicate.TargetExists"), FString::Printf(
			TEXT("Target '%s' already exists; choose a new name (find_assets lists what exists)."), *Target));
	}
	return {};
}

TOptional<FAssetDocumentResult> RejectSource(const FAssetDocumentDuplicateRequest& Request, const UObject* Source)
{
	if (Source == nullptr)
	{
		return MakeDuplicateFailure(Request, TEXT("/source"), TEXT("Duplicate.SourceNotFound"), FString::Printf(
			TEXT("No asset at '%s'. find_assets lists existing assets (project, engine and plugin content) by path, class and name."),
			*Request.Source.TrimStartAndEnd()));
	}
	if (Source->IsA<UObjectRedirector>() || !Source->IsAsset())
	{
		return MakeDuplicateFailure(Request, TEXT("/source"), TEXT("Duplicate.UnsupportedSource"), FString::Printf(
			TEXT("'%s' (%s) cannot be duplicated: it is a redirector, not an asset (find_assets lists the asset it points to)."),
			*Source->GetPathName(), *Source->GetClass()->GetName()));
	}
	if (const UWorld* World = Cast<UWorld>(Source))
	{
		if (World->IsPartitionedWorld() && IsDuplicateSourceWorldInUse(*World))
		{
			// As the Content Browser (UAssetDefinition_World::CanDuplicate): the open World Partition map has
			// actors outside memory that a copy of the loaded world would lose.
			return MakeDuplicateFailure(Request, TEXT("/source"), TEXT("Duplicate.UnsupportedSource"), FString::Printf(
				TEXT("'%s' is a World Partition map open in the editor; the editor cannot duplicate a partition world while it is "
					"used. Open another map first (open_level / new_level), then duplicate it."), *World->GetPackage()->GetName()));
		}
	}
	return {};
}

// The packages under a map's external actor and external object folders on disk (one-file-per-actor maps).
TArray<FString> MapExternalPackagesOnDisk(const FString& MapPackage)
{
	TArray<FString> Packages;
	for (const FString& Root : {ULevel::GetExternalActorsPath(MapPackage), FExternalPackageHelper::GetExternalObjectsPath(MapPackage)})
	{
		FString Directory;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Root, Directory))
		{
			continue;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, *(TEXT("*") + FPackageName::GetAssetPackageExtension()), true, false);
		for (const FString& File : Files)
		{
			FString Package;
			if (FPackageName::TryConvertFilenameToLongPackageName(File, Package))
			{
				Packages.Add(Package);
			}
		}
	}
	Packages.Sort();
	return Packages;
}

// The Content Browser's Duplicate of a map (ObjectTools::DuplicateSingleObject in world mode) copies the world and
// the external packages of the actors loaded in its level. A partitioned map that is not open has its actors on disk
// only: they are loaded into the source world first, as the editor's Save As of such a map does
// (UWorldPartition::LoadAllActors), and kept loaded by the returned references until the copy is saved.
TArray<FWorldPartitionReference> LoadAllMapActors(UWorld& World)
{
	TArray<FWorldPartitionReference> References;
	UWorldPartition* Partition = World.GetWorldPartition();
	if (Partition != nullptr && Partition->IsInitialized() && !IsDuplicateSourceWorldInUse(World))
	{
		Partition->LoadAllActors(References);
	}
	return References;
}

// The Content Browser's Duplicate of an asset whose class registered a duplicate fixup (AssetDocumentDuplicateFixups.h):
// the steps of IAssetTools::DuplicateAsset for an asset that is not a map (ObjectTools::DuplicateSingleObject), with
// the class's fixup between the copy and its post-loads.
UObject* DuplicateWithFixups(UObject& Source, const FString& Target)
{
	// The source is not selected while it is copied (as the editor's duplicate does); its selection is kept.
	USelection* Selection = GEditor != nullptr ? GEditor->GetSelectedObjects() : nullptr;
	const bool bSelected = Selection != nullptr && Selection->IsSelected(&Source);
	if (bSelected)
	{
		Selection->Deselect(&Source);
	}
	UPackage* Package = CreatePackage(*Target);
	FObjectDuplicationParameters Parameters = InitStaticDuplicateObjectParams(
		&Source, Package, *FPackageName::GetLongPackageAssetName(Target), RF_AllFlags, nullptr, EDuplicateMode::Normal);
	UObject* Copy = DuplicateObjectWithAssetDocumentFixups(Parameters);
	if (bSelected)
	{
		Selection->Select(&Source);
	}
	if (Copy == nullptr)
	{
		return nullptr;
	}
	TMap<UObject*, UObject*> Replacements;
	Replacements.Add(&Source, Copy);
	Copy->MarkPackageDirty();
	if (Source.GetOutermost()->HasAnyPackageFlags(PKG_DisallowExport))
	{
		Package->SetPackageFlags(PKG_DisallowExport);
	}
	Package->SetAssetAccessSpecifier(Source.GetOutermost()->GetAssetAccessSpecifier());
	FAssetRegistryModule::AssetCreated(Copy);
	FArchiveReplaceObjectRef<UObject> ReplaceReferences(Copy, Replacements,
		EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
	Copy->SetFlags(RF_Public | RF_Standalone);
	return Copy;
}

// Every package of a duplicated map (the map and its external actor / object packages).
TArray<UPackage*> DuplicatedMapPackages(const UObject& Copy)
{
	TArray<UPackage*> Packages = {Copy.GetPackage()};
	for (UPackage* External : Copy.GetPackage()->GetExternalPackages())
	{
		Packages.AddUnique(External);
	}
	return Packages;
}
}

namespace UE::AssetDocument
{
bool IsReadOnlyContentPath(const FString& PackageName)
{
	return !PackageName.StartsWith(GameRoot) && FPackageName::IsValidLongPackageName(PackageName, false);
}

FAssetDocumentResult DuplicateAsset(const FAssetDocumentDuplicateRequest& Request)
{
	const FString Target = PackageNameOf(Request.Target);
	if (TOptional<FAssetDocumentResult> Rejected = RejectTarget(Request, Target))
	{
		return MoveTemp(*Rejected);
	}
	UObject* Source = FindSourceAsset(Request.Source);
	if (TOptional<FAssetDocumentResult> Rejected = RejectSource(Request, Source))
	{
		return MoveTemp(*Rejected);
	}

	UWorld* SourceWorld = Cast<UWorld>(Source);
	TArray<FString> SourceExternal;
	TArray<FWorldPartitionReference> LoadedActors;
	int32 SourceLoaded = 0;
	if (SourceWorld != nullptr)
	{
		LoadedActors = LoadAllMapActors(*SourceWorld);
		SourceExternal = MapExternalPackagesOnDisk(SourceWorld->GetPackage()->GetName());
		SourceLoaded = SourceWorld->GetPackage()->GetExternalPackages().Num();
	}

	UObject* Copy = nullptr;
	if (SourceWorld == nullptr && HasAssetDocumentDuplicateFixup(*Source->GetClass()))
	{
		Copy = DuplicateWithFixups(*Source, Target);
	}
	else
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Copy = AssetTools.DuplicateAsset(
			FPackageName::GetLongPackageAssetName(Target), FPackageName::GetLongPackagePath(Target), Source);
	}
	if (Copy == nullptr)
	{
		DiscardUnsavedPackage(Target);
		return MakeDuplicateFailure(Request, TEXT("/source"), TEXT("Duplicate.Failed"), FString::Printf(
			TEXT("The editor could not duplicate '%s' to '%s' (see the editor log)."), *Source->GetPathName(), *Target));
	}
	// A map is its package plus the external packages of its actors and objects (one file per actor).
	const TArray<UPackage*> Packages = SourceWorld != nullptr ? DuplicatedMapPackages(*Copy) : TArray<UPackage*>({Copy->GetPackage()});
	const auto Discard = [&Packages]()
	{
		AssetDocumentAssetCompilation::FinishPackageCompilation(Packages);
		for (UPackage* Package : Packages)
		{
			const bool bMap = Package == Packages[0] && Package->ContainsMap();
			const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(),
				bMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
			ResetLoaders(Package);
			IFileManager::Get().Delete(*Filename, false, true, true);
		}
		for (UPackage* Package : Packages)
		{
			DiscardDuplicatePackage(*Package);
		}
	};
	// A map that is not open was loaded from disk: its copy must hold every external package it has there.
	if (SourceWorld != nullptr && !IsDuplicateSourceWorldInUse(*SourceWorld) && Packages.Num() - 1 < SourceExternal.Num())
	{
		const int32 Copied = Packages.Num() - 1;
		Discard();
		return MakeDuplicateFailure(Request, TEXT("/source"), TEXT("Duplicate.Failed"), FString::Printf(
			TEXT("Duplicating '%s' copied %d of its %d external actor / object packages (%d loaded), so the copy would lose "
				"actors; it was discarded (see the editor log)."), *Source->GetPathName(), Copied, SourceExternal.Num(), SourceLoaded));
	}
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false) || !FPackageName::DoesPackageExist(Target))
	{
		Discard();
		return MakeDuplicateFailure(Request, TEXT("/target"), TEXT("Duplicate.SaveFailed"), FString::Printf(
			TEXT("'%s' was duplicated but could not be saved; the copy was discarded (see the editor log)."), *Target));
	}

	FAssetDocumentResult Result = FAssetDocumentResult::Success(FString::Printf(
		TEXT("Duplicated '%s' to '%s' and saved it (as the Content Browser's Duplicate: what it references is not copied)"),
		*Source->GetPathName(), *Copy->GetPathName()));
	Result.Target = Target;
	Result.AssetPath = Copy->GetPathName();
	Result.bSavedAsset = true;
	Result.Payload = MakeShared<FJsonObject>();
	Result.Payload->SetStringField(TEXT("source"), Source->GetPathName());
	Result.Payload->SetStringField(TEXT("class"), Copy->GetClass()->GetPathName());
	if (SourceWorld != nullptr)
	{
		// The map's external actor / object packages (one file per actor) copied with it.
		Result.Payload->SetNumberField(TEXT("external_packages"), Packages.Num() - 1);
	}
	Result.Compile = Discovery::DescribeCompileStatus(*Copy);
	return Result;
}
}
