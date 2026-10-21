// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Runtime/AssetDocumentPackageStaging.h"

#include "AssetDocumentAtomicFile.h"
#include "AssetDocumentAssetCompilation.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentPackageSave.h"
#include "Runtime/AssetDocumentSidecarByteIo.h"
#include "Runtime/AssetDocumentStagedPackagePurge.h"
#include "Serialization/ArchiveUObject.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include "Containers/StringConv.h"
#include <sys/stat.h>
#endif

namespace
{
FAssetDocumentContractValidationResult MakePackageResult(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Persistence/Package");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

FAssetDocumentContractValidationResult MakeValidPackageResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
// The objects an object's saved data refers to, collected the way a package save harvests its exports: the object is
// serialized for saving (references only, bulk data skipped) and each object reference is recorded.
class FSavedReferenceCollector final : public FArchiveUObject
{
public:
	explicit FSavedReferenceCollector(TArray<UObject*>& InReferences)
		: References(InReferences)
	{
		SetIsSaving(true);
		SetIsPersistent(true);
		ArIsObjectReferenceCollector = true;
		ArShouldSkipBulkData = true;
	}

	using FArchiveUObject::operator<<;

	virtual FArchive& operator<<(UObject*& Object) override
	{
		if (Object != nullptr)
		{
			References.Add(Object);
		}
		return *this;
	}

	virtual FString GetArchiveName() const override
	{
		return TEXT("FSavedReferenceCollector");
	}

private:
	TArray<UObject*>& References;
};

// For the length of a save, the objects of the package that the save does not write are transient. A package keeps
// objects an edit replaced without destroying them (e.g. the copy of its parent that a duplicated Niagara emitter
// keeps for merging, which the duplicate replaces with a fresh copy). The save's localization gather visits every
// object of the package, written or not, and this engine refuses to save a package whose localizable text sits only
// in objects the save does not write. Transient objects are skipped by the gather as by the save, so the save writes
// the same file and succeeds. The objects are restored when the save ends: an undo can make them referenced again.
// What the save writes is judged before the save routes PreSave; a type whose PreSave changes references makes that
// change in PackageSave::OnBeforeSave, which runs first.
class FUnsavedObjectsTransientScope
{
public:
	FUnsavedObjectsTransientScope(UPackage& Package, UObject* Asset, EObjectFlags TopLevelFlags)
	{
		TArray<UObject*> Objects;
		GetObjectsWithPackage(&Package, Objects, AssetDocumentObjects::IncludeNested, RF_Transient, EInternalObjectFlags::Garbage);
		// What the save writes: the asset, the objects with the top-level flags and the objects the package's metadata
		// names, and from them every object of the package that saved data, an outer or an archetype refers to.
		TSet<UObject*> Saved;
		TArray<UObject*> Pending;
		const auto Reach = [&Saved, &Pending, &Package](UObject* Object)
		{
			if (Object != nullptr && Object->GetPackage() == &Package && !Object->HasAnyFlags(RF_Transient)
				&& !Saved.Contains(Object))
			{
				Saved.Add(Object);
				Pending.Add(Object);
			}
		};
		Reach(Asset);
		for (UObject* Object : Objects)
		{
			if (Object->HasAnyFlags(TopLevelFlags))
			{
				Reach(Object);
			}
		}
#if WITH_METADATA
		{
			TArray<UObject*> References;
			FSavedReferenceCollector Collector(References);
			Collector << Package.GetMetaData();
			for (UObject* Reference : References)
			{
				Reach(Reference);
			}
		}
#endif
		while (!Pending.IsEmpty())
		{
			UObject* Object = Pending.Pop(EAllowShrinking::No);
			Reach(Object->GetOuter());
			Reach(Object->GetArchetype());
			TArray<UObject*> References;
			FSavedReferenceCollector Collector(References);
			Object->Serialize(Collector);
			for (UObject* Reference : References)
			{
				Reach(Reference);
			}
		}
		for (UObject* Object : Objects)
		{
			if (Object->GetPackage() == &Package && !Saved.Contains(Object))
			{
				Object->SetFlags(RF_Transient);
				Unsaved.Add(Object);
			}
		}
	}

	~FUnsavedObjectsTransientScope()
	{
		for (const TWeakObjectPtr<UObject>& Object : Unsaved)
		{
			if (UObject* Restored = Object.Get())
			{
				Restored->ClearFlags(RF_Transient);
			}
		}
	}

	FUnsavedObjectsTransientScope(const FUnsavedObjectsTransientScope&) = delete;
	FUnsavedObjectsTransientScope& operator=(const FUnsavedObjectsTransientScope&) = delete;

private:
	TArray<TWeakObjectPtr<UObject>> Unsaved;
};
#endif

FString CanonicalPackagePath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(MoveTemp(Path));
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString ResolveTargetHeaderPath(const FString& TargetPackagePath)
{
	if (FPackageName::IsValidLongPackageName(TargetPackagePath))
	{
		return CanonicalPackagePath(FPackageName::LongPackageNameToFilename(
			TargetPackagePath, FPackageName::GetAssetPackageExtension()));
	}
	return CanonicalPackagePath(TargetPackagePath);
}

FString MakeAggregateDigest(const TArray<FAssetDocumentPackageArtifact>& Artifacts)
{
	FString Manifest;
	for (const FAssetDocumentPackageArtifact& Artifact : Artifacts)
	{
		Manifest += Artifact.TargetPath + TEXT("\n") + Artifact.ContentDigest + TEXT("\n");
	}
	FTCHARToUTF8 Utf8(*Manifest);
	return ComputeAssetDocumentContentDigest(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

bool CapturePackageArtifact(
	const FAssetDocumentPackageArtifact& Artifact,
	const FString& BackupPath,
	FAssetDocumentJournalArtifact& OutJournalArtifact)
{
	OutJournalArtifact.TargetPath = Artifact.TargetPath;
	OutJournalArtifact.BackupPath = BackupPath;
	OutJournalArtifact.InstalledContentDigest = Artifact.ContentDigest;
	OutJournalArtifact.bTargetExisted = IFileManager::Get().FileExists(*Artifact.TargetPath);
	if (!OutJournalArtifact.bTargetExisted)
	{
		return true;
	}
	TArray64<uint8> Bytes;
	FString Error;
	if (!FFileHelper::LoadFileToArray(Bytes, *Artifact.TargetPath)
		|| !FAssetDocumentAtomicFile::WriteBytesAtomically(BackupPath, Bytes, Error))
	{
		return false;
	}
	OutJournalArtifact.PreimageContentDigest = ComputeAssetDocumentContentDigest(Bytes);
	return true;
}

bool IsStagedCompanion(const FString& Filename, const FString& HeaderStem)
{
	return FPaths::GetBaseFilename(Filename) == HeaderStem;
}

void TryAppendStagedArtifact(
	const TCHAR* Path,
	bool bDirectory,
	const FString& HeaderStem,
	const FString& TargetStem,
	FAssetDocumentPackageArtifactSet& OutArtifacts)
{
	if (bDirectory)
	{
		return;
	}
	if (!IsStagedCompanion(Path, HeaderStem))
	{
		return;
	}
	TArray64<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, Path))
	{
		return;
	}
	FAssetDocumentPackageArtifact& Artifact = OutArtifacts.Artifacts.AddDefaulted_GetRef();
	Artifact.StagedPath = Path;
	Artifact.TargetPath = TargetStem + FPaths::GetExtension(Path, true);
	Artifact.ContentDigest = ComputeAssetDocumentContentDigest(Bytes);
}

FAssetDocumentContractValidationResult DiscoverSavedArtifacts(
	const FString& StagedHeaderPath,
	const FString& TargetHeaderPath,
	FAssetDocumentPackageArtifactSet& OutArtifacts)
{
	const FString HeaderStem = FPaths::GetBaseFilename(StagedHeaderPath);
	const FString TargetStem = FPaths::GetBaseFilename(TargetHeaderPath, false);
	OutArtifacts.Artifacts.Reset();
	IFileManager::Get().IterateDirectory(*FPaths::GetPath(StagedHeaderPath), [&](const TCHAR* Path, bool bDirectory)
	{
		TryAppendStagedArtifact(Path, bDirectory, HeaderStem, TargetStem, OutArtifacts);
		return true;
	});
	if (OutArtifacts.Artifacts.IsEmpty())
	{
		return MakePackageResult(TEXT("Persistence.StagePackageFailed"), TEXT("Saved package artifacts could not be discovered."));
	}
	return MakeValidPackageResult();
}

bool IsActualPackageSymlink(IPlatformFile& PlatformFile, const FString& Path)
{
#if PLATFORM_MAC || PLATFORM_LINUX
	struct stat FileInfo;
	FTCHARToUTF8 NativePath(*Path);
	return lstat(NativePath.Get(), &FileInfo) == 0 && S_ISLNK(FileInfo.st_mode);
#else
	return PlatformFile.IsSymlink(*Path) == ESymlinkResult::Symlink;
#endif
}

bool HasPackageSymlinkComponent(const FString& AbsolutePath)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FString Cursor = AbsolutePath;
	while (!Cursor.IsEmpty() && Cursor != FPaths::GetPath(Cursor))
	{
		if (IsActualPackageSymlink(PlatformFile, Cursor))
		{
			return true;
		}
		Cursor = FPaths::GetPath(Cursor);
	}
	return false;
}

bool IsRecognizedCompanionSuffix(FStringView Suffix)
{
	const FStringView Recognized[] = {
		TEXTVIEW(".uexp"), TEXTVIEW(".ubulk"), TEXTVIEW(".uptnl"),
		TEXTVIEW(".m.ubulk"), TEXTVIEW(".upayload")};
	for (FStringView Candidate : Recognized)
	{
		if (Suffix == Candidate)
		{
			return true;
		}
	}
	return false;
}

TOptional<FString> GetPackageArtifactSuffix(
	const FString& TargetPath,
	const FString& TargetHeaderPath)
{
	if (TargetPath == TargetHeaderPath)
	{
		return FString();
	}
	const FString HeaderExtension = FPaths::GetExtension(TargetHeaderPath, true);
	const FString TargetStem = TargetHeaderPath.LeftChop(HeaderExtension.Len());
	if (!TargetPath.StartsWith(TargetStem, ESearchCase::CaseSensitive))
	{
		return {};
	}
	const FString Suffix = TargetPath.RightChop(TargetStem.Len());
	return IsRecognizedCompanionSuffix(Suffix) ? TOptional<FString>(Suffix) : TOptional<FString>();
}

FString ExpectedStagedPath(const FString& StagedHeaderPath, const FString& Suffix)
{
	if (Suffix.IsEmpty())
	{
		return StagedHeaderPath;
	}
	const FString HeaderExtension = FPaths::GetExtension(StagedHeaderPath, true);
	return StagedHeaderPath.LeftChop(HeaderExtension.Len()) + Suffix;
}

bool IsAuthorizedArtifact(
	const FAssetDocumentPackageArtifact& Artifact,
	const FString& StagingRoot,
	const FString& StagedHeaderPath,
	const FString& TargetHeaderPath)
{
	const TOptional<FString> Suffix = GetPackageArtifactSuffix(Artifact.TargetPath, TargetHeaderPath);
	if (!Suffix.IsSet())
	{
		return false;
	}
	if (HasPackageSymlinkComponent(Artifact.StagedPath) || HasPackageSymlinkComponent(Artifact.TargetPath))
	{
		return false;
	}
	if (!FPaths::IsUnderDirectory(Artifact.StagedPath, StagingRoot))
	{
		return false;
	}
	return Artifact.StagedPath == ExpectedStagedPath(StagedHeaderPath, Suffix.GetValue());
}

FAssetDocumentContractValidationResult FinalizeArtifacts(
	const FString& StagingRoot,
	const FString& StagedHeaderPath,
	const FString& TargetHeaderPath,
	FAssetDocumentPackageArtifactSet& OutArtifacts)
{
	if (OutArtifacts.Artifacts.IsEmpty())
	{
		return MakePackageResult(TEXT("Persistence.StagePackageFailed"), TEXT("Package save emitted no staged artifacts."));
	}
	for (FAssetDocumentPackageArtifact& Artifact : OutArtifacts.Artifacts)
	{
		Artifact.StagedPath = CanonicalPackagePath(MoveTemp(Artifact.StagedPath));
		Artifact.TargetPath = CanonicalPackagePath(MoveTemp(Artifact.TargetPath));
		if (!IsAuthorizedArtifact(Artifact, StagingRoot, StagedHeaderPath, TargetHeaderPath))
		{
			return MakePackageResult(TEXT("Persistence.StagePackageFailed"),
				TEXT("Package save emitted an artifact outside the request authority."));
		}
	}
	OutArtifacts.Artifacts.Sort([](const FAssetDocumentPackageArtifact& Left, const FAssetDocumentPackageArtifact& Right)
	{
		return Left.TargetPath < Right.TargetPath;
	});
	OutArtifacts.AggregateContentDigest = MakeAggregateDigest(OutArtifacts.Artifacts);
	return MakeValidPackageResult();
}

// Fresh verification loads the staged header as an ordinary package (its name is the /Temp/ mount
// of the staging file). It is a request-private copy: nothing may keep it alive after the request,
// and it must not keep alive what it references (a staged child material pins its parent material).
// Release it: detach the linker (which also closes the file on Windows) and drop Root, Standalone
// and Public, so the next garbage collection frees it once nothing references it; the live target
// and other packages are not touched. The outermost request scope then collects it
// (AssetDocumentStagedPackagePurge.h), so no provider sees it after the request. It is not marked
// garbage: until that collection the copy is still a valid object that editor-wide iterators can
// reach (an enumerator edit recompiles every user-defined struct with a property of that enum, and
// recompiling a garbage struct trips the engine's DefaultStructInstance ensure because weak
// pointers to it are already invalid).
void ReleaseLoadedStagedPackage(const FString& StagedHeaderPath)
{
	const FString Extension = FPaths::GetExtension(StagedHeaderPath, true);
	if (Extension != FPackageName::GetAssetPackageExtension() && Extension != FPackageName::GetMapPackageExtension())
	{
		return;
	}
	FString PackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(StagedHeaderPath, PackageName)
		|| !PackageName.StartsWith(TEXT("/Temp/"), ESearchCase::CaseSensitive))
	{
		return;
	}
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package == nullptr)
	{
		return;
	}
	FlushAsyncLoading();
	AssetDocumentAssetCompilation::FinishPackageCompilation({Package});
	ResetLoaders(Package);
	auto Retire = [](UObject* Object)
	{
		if (Object->IsRooted())
		{
			Object->RemoveFromRoot();
		}
		Object->ClearFlags(RF_Public | RF_Standalone);
	};
	ForEachObjectWithPackage(Package, [&Retire](UObject* Object)
	{
		Retire(Object);
		return true;
	});
	Package->SetDirtyFlag(false);
	Retire(Package);
	UE::AssetDocument::StagedPackages::NoteReleased(*Package);
}

bool IsBoundPackageTarget(
	UPackage& Package,
	UObject* Asset,
	const FString& TargetHeaderPath)
{
	if (Asset == nullptr || Asset->GetPackage() != &Package)
	{
		return false;
	}
	return ResolveTargetHeaderPath(Package.GetName()) == CanonicalPackagePath(TargetHeaderPath);
}

bool CanSavePackage(
	UPackage& Package,
	UObject* Asset,
	const FString& StagedHeaderPath,
	const FString& TargetHeaderPath)
{
	if (!IsBoundPackageTarget(Package, Asset, TargetHeaderPath))
	{
		return false;
	}
	return IFileManager::Get().MakeDirectory(*FPaths::GetPath(StagedHeaderPath), true);
}
}

FAssetDocumentPackageStager::FAssetDocumentPackageStager(IAssetDocumentPackageSaveBackend& InBackend)
	: Backend(InBackend)
{
}

FAssetDocumentContractValidationResult FAssetDocumentPackageStager::Stage(
	const FAssetDocumentPersistencePlan& Plan,
	FAssetDocumentPackageArtifactSet& OutArtifacts)
{
	const FString TargetHeaderPath = ResolveTargetHeaderPath(Plan.TargetPackagePath);
	if (Plan.StagingRoot.IsEmpty() || TargetHeaderPath.IsEmpty())
	{
		return MakePackageResult(TEXT("Persistence.StagePackageFailed"), TEXT("Package staging input is incomplete."));
	}
	const FString StagingRoot = CanonicalPackagePath(Plan.StagingRoot);
	const FString StagedHeaderPath = CanonicalPackagePath(
		FPaths::Combine(StagingRoot, FPaths::GetCleanFilename(TargetHeaderPath)));
	if (HasPackageSymlinkComponent(StagingRoot))
	{
		return MakePackageResult(TEXT("Persistence.StagePackageFailed"),
			TEXT("Package staging root escapes the request authority."));
	}
	FAssetDocumentContractValidationResult Result = Backend.SaveToStaging(StagedHeaderPath, TargetHeaderPath, OutArtifacts);
	if (!Result.bValid)
	{
		return Result;
	}
	return FinalizeArtifacts(StagingRoot, StagedHeaderPath, TargetHeaderPath, OutArtifacts);
}

FAssetDocumentContractValidationResult FAssetDocumentPackageStager::CaptureInstalledBaseline(
	const FAssetDocumentPackageArtifactSet& Artifacts,
	const FString& BackupRoot,
	TArray<FAssetDocumentJournalArtifact>& OutJournalArtifacts)
{
	// Windows CreateDirectory reports "already exists" for a regular file too, so MakeDirectory alone
	// cannot prove the backup root is a usable directory.
	if (!IFileManager::Get().MakeDirectory(*BackupRoot, true) || !IFileManager::Get().DirectoryExists(*BackupRoot))
	{
		return MakePackageResult(TEXT("Persistence.PrepareJournalFailed"), TEXT("Package backup root could not be created."));
	}
	OutJournalArtifacts.Reset();
	for (int32 Index = 0; Index < Artifacts.Artifacts.Num(); ++Index)
	{
		FAssetDocumentJournalArtifact& JournalArtifact = OutJournalArtifacts.AddDefaulted_GetRef();
		const FString BackupPath = FPaths::Combine(BackupRoot, FString::Printf(TEXT("package-%04d.bin"), Index));
		if (!CapturePackageArtifact(Artifacts.Artifacts[Index], BackupPath, JournalArtifact))
		{
			return MakePackageResult(TEXT("Persistence.PrepareJournalFailed"), TEXT("Package baseline capture failed."));
		}
	}
	return MakeValidPackageResult();
}

void FAssetDocumentPackageStager::CleanupStaged(
	const FAssetDocumentPackageArtifactSet& Artifacts,
	FStringView StagingRoot)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FString(StagingRoot));
	for (const FAssetDocumentPackageArtifact& Artifact : Artifacts.Artifacts)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(Artifact.StagedPath);
		if (FPaths::IsUnderDirectory(Path, Root))
		{
			// Success, failure and rollback all end here: release the copy the verifier loaded
			// (if any) before its files go. Windows cannot delete a file a linker holds open.
			ReleaseLoadedStagedPackage(Path);
			FAssetDocumentAtomicFile::ReleaseLoadedPackageFileHandles(Path);
			IFileManager::Get().Delete(*Path, false, true, true);
		}
	}
}

namespace UE::AssetDocument::PackageSave
{
FOnBeforeSave& OnBeforeSave()
{
	static FOnBeforeSave Delegate;
	return Delegate;
}
} // namespace UE::AssetDocument::PackageSave

FAssetDocumentPublicPackageSaveBackend::FAssetDocumentPublicPackageSaveBackend(
	UPackage& InPackage,
	UObject* InAsset)
	: Package(InPackage)
	, Asset(InAsset)
{
}

FAssetDocumentContractValidationResult FAssetDocumentPublicPackageSaveBackend::SaveToStaging(
	const FString& StagedHeaderPath,
	const FString& TargetHeaderPath,
	FAssetDocumentPackageArtifactSet& OutArtifacts)
{
	if (!CanSavePackage(Package, Asset, StagedHeaderPath, TargetHeaderPath))
	{
		return MakePackageResult(TEXT("Persistence.StagePackageFailed"), TEXT("Request-scoped package save input is invalid."));
	}
	FSavePackageArgs SaveArgs;
	// Save admission/serialization failures must return through the transaction's
	// rollback path; the default GError device terminates the Editor instead.
	SaveArgs.Error = GWarn;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_FromAutosave | SAVE_KeepDirty;
	SaveArgs.bSlowTask = false;
	UE::AssetDocument::PackageSave::OnBeforeSave().Broadcast(Package);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	const FUnsavedObjectsTransientScope UnsavedObjects(Package, Asset, SaveArgs.TopLevelFlags);
#endif
	if (!UPackage::SavePackage(&Package, Asset, *StagedHeaderPath, SaveArgs))
	{
		return MakePackageResult(TEXT("Persistence.StagePackageFailed"), TEXT("Authoritative package save failed."));
	}
	return DiscoverSavedArtifacts(StagedHeaderPath, TargetHeaderPath, OutArtifacts);
}
