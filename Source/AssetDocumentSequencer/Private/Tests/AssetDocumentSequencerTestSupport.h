// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Shared fixtures of the Sequencer automation tests: /Game targets, the corpus (design §6.1: template folders copied into
// /Game at their own paths, as creating a project from the template does, so the sequences' references and locators
// stay valid; optional plugin content), a Level Sequence built with the editor's own calls, Service calls and
// comparisons.

#pragma once

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "../../../AssetDocument/Private/Tests/AssetDocumentDiffCountTestSupport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "LevelSequence.h"
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
class UMovieSceneSection;
class UMovieSceneTrack;

namespace AssetDocumentSequencerTests
{
const TCHAR* const FolderName = TEXT("AssetDocumentSequencer");
const TCHAR* const LevelSequenceClass = TEXT("/Script/LevelSequence.LevelSequence");

inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentSequencerTests", "Cleanup", "AssetDocument Sequencer test cleanup"));
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
	ULevelSequence* GetSequence() const { return Cast<ULevelSequence>(Get()); }

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

// One folder (or file) of the engine install's content copied into the project's Content: Source is a content folder
// relative to the engine root (Templates/<T>/Content maps to /Game), Relative the path inside it ("DemoTemplate/Sequencer",
// "Main.umap"), Mount the /Game folder of a shared template pack (Templates/TemplateResources/<Level>/<Mount>/Content
// maps to /Game/<Mount>).
struct FContentCopy
{
	const TCHAR* Source;
	const TCHAR* Relative;
	const TCHAR* Mount = nullptr;
};

// The copies of one test; a target that exists already is left alone (and not removed). Only what this test copied
// is unloaded and deleted again, with the directories it created.
struct FContentCopies
{
	TArray<FString> CopiedFiles;
	TArray<FString> CopiedPackages;
	TArray<FString> CreatedDirectories;
	TArray<FString> Skipped;

	explicit FContentCopies(std::initializer_list<FContentCopy> Copies)
	{
		const FString ProjectContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
		for (const FContentCopy& Copy : Copies)
		{
			const FString MountDir = Copy.Mount != nullptr ? FString(Copy.Mount) : FString();
			const FString ContentDir = MountDir.IsEmpty() ? ProjectContentDir : FPaths::Combine(ProjectContentDir, MountDir);
			const FString SourceRoot = FPaths::Combine(FPaths::RootDir(), Copy.Source);
			const FString From = FPaths::Combine(SourceRoot, Copy.Relative);
			const FString To = FPaths::Combine(ContentDir, Copy.Relative);
			const bool bFile = IFileManager::Get().FileExists(*From);
			if ((!bFile && !IFileManager::Get().DirectoryExists(*From)) || IFileManager::Get().FileExists(*To) || IFileManager::Get().DirectoryExists(*To))
			{
				Skipped.Add(Copy.Relative);
				continue;
			}
			// The topmost directory this copy creates (removed afterwards).
			FString Created = bFile ? FPaths::GetPath(To) : To;
			for (FString Parent = FPaths::GetPath(Created); Parent.Len() > ProjectContentDir.Len() && !IFileManager::Get().DirectoryExists(*Parent);
				Parent = FPaths::GetPath(Parent))
			{
				Created = Parent;
			}
			if (Created.Len() > ProjectContentDir.Len() && !IFileManager::Get().DirectoryExists(*Created))
			{
				CreatedDirectories.Add(Created);
			}
			TArray<FString> Files;
			if (bFile)
			{
				Files.Add(From);
			}
			else
			{
				IFileManager::Get().FindFilesRecursive(Files, *From, TEXT("*.*"), true, false);
			}
			for (const FString& File : Files)
			{
				FString Relative = File;
				FPaths::MakePathRelativeTo(Relative, *(SourceRoot + TEXT("/")));
				const FString Target = FPaths::Combine(ContentDir, Relative);
				if (IFileManager::Get().Copy(*Target, *File) == COPY_OK)
				{
					CopiedFiles.Add(Target);
					const FString Extension = FPaths::GetExtension(Relative);
					if (Extension == TEXT("uasset") || Extension == TEXT("umap"))
					{
						const FString InGame = FPaths::Combine(FPaths::GetPath(Relative), FPaths::GetBaseFilename(Relative));
						CopiedPackages.Add(FString(TEXT("/Game/")) + (MountDir.IsEmpty() ? InGame : MountDir + TEXT("/") + InGame));
					}
				}
			}
		}
		if (!CopiedFiles.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanFilesSynchronous(CopiedFiles, true);
		}
	}

	FContentCopies(const FContentCopies&) = delete;
	FContentCopies& operator=(const FContentCopies&) = delete;

	~FContentCopies()
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

inline bool PluginEnabled(const TCHAR* Name)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
	return Plugin.IsValid() && Plugin->IsEnabled();
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

inline FString FirstPath(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	const FAssetDocumentDiagnostic* Found = Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	return Found != nullptr ? Found->Path : FString();
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

// Unloads the package of the object and loads it again from disk.
inline UObject* Reload(const FString& ObjectPath)
{
	if (UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject())
	{
		ReleaseUndoHistory();
		Asset->GetOutermost()->ClearDirtyFlag();
		UPackageTools::UnloadPackages({Asset->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	return LoadQuiet(ObjectPath);
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

inline FString StatusName(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Extracted.Compile->GetStringField(TEXT("Status")) : FString(TEXT("-"));
}

inline FString StatusOf(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Serialize(Extracted.Compile) : FString(TEXT("-"));
}

// The status messages of one severity: their document paths.
inline TArray<FString> StatusPaths(const FAssetDocumentResult& Result, const TCHAR* Severity)
{
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (Result.Compile.IsValid() && Result.Compile->TryGetArrayField(TEXT("Messages"), Messages))
	{
		for (const TSharedPtr<FJsonValue>& Message : *Messages)
		{
			FString Path;
			if (Message->AsObject()->GetStringField(TEXT("Severity")) == Severity && Message->AsObject()->TryGetStringField(TEXT("Path"), Path))
			{
				Paths.Add(Path);
			}
		}
	}
	return Paths;
}

// Read warnings of an Inspect payload ({Code, Message, Path}).
inline TArray<TSharedPtr<FJsonObject>> ReadWarnings(const FAssetDocumentResult& Inspected)
{
	TArray<TSharedPtr<FJsonObject>> Out;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Inspected.Payload.IsValid() && Inspected.Payload->TryGetArrayField(TEXT("ReadWarnings"), Rows))
	{
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			Out.Add(Row->AsObject());
		}
	}
	return Out;
}

// The stable read diagnostics of design §3.9 (and the read's own); anything else is an unknown diagnostic.
inline bool IsKnownReadCode(const FString& Code)
{
	static const TCHAR* const Known[] = {TEXT("Sequencer.UnsupportedValue"), TEXT("Sequencer.UnsupportedChannel"),
		TEXT("Sequencer.UnsupportedChannelOverride"), TEXT("Sequencer.UnsupportedRemoteBinding"),
		TEXT("Sequencer.UnsupportedBindingShape"), TEXT("Sequencer.UnsupportedLocator"), TEXT("Sequencer.ControlRigTrack"),
		TEXT("Sequencer.DuplicateChannelName")};
	for (const TCHAR* Item : Known)
	{
		if (Code == Item)
		{
			return true;
		}
	}
	return false;
}

struct FCorpusEntry
{
	const TCHAR* Path;
	const TCHAR* Plugin; // the plugin the asset needs (nullptr: none)
	const TCHAR* Why;
};

// Design §6.1 (#1-#12). Template content: FContentCopies of CorpusCopies(); plugin content: read from its mount.
const FCorpusEntry Corpus[] = {
	{TEXT("/Game/DemoTemplate/Sequencer/NewLevelSequence.NewLevelSequence"), nullptr, TEXT("#1 spawnable + possessable, spawn, camera cut, double channels")},
	{TEXT("/Game/Studios/Sequences/SEQ_Orbit_Studio_Dark.SEQ_Orbit_Studio_Dark"), nullptr, TEXT("#2 transforms, camera floats, bool property")},
	{TEXT("/Game/Studios/Sequences/SEQ_Truck_Studio_White.SEQ_Truck_Studio_White"), nullptr, TEXT("#3 smaller #2")},
	{TEXT("/Game/ProductAssets/Sequences/Camera1_Seq.Camera1_Seq"), nullptr, TEXT("#4 4.27-era upgrade, bool track, look-at struct")},
	{TEXT("/Game/ArchvisProject/Sequences/Cameras/Seq_CineCameraActor01.Seq_CineCameraActor01"), nullptr, TEXT("#5 fade, camera tracks")},
	{TEXT("/Game/ArchvisProject/Sequences/Cameras/Seq_CineCameraActor02.Seq_CineCameraActor02"), nullptr, TEXT("#5")},
	{TEXT("/Game/ArchvisProject/Sequences/Cameras/Seq_CineCameraActor03.Seq_CineCameraActor03"), nullptr, TEXT("#5")},
	{TEXT("/Game/ArchvisProject/Sequences/Cameras/Seq_Master_Cameras.Seq_Master_Cameras"), nullptr, TEXT("#6 cinematic shot track")},
	{TEXT("/Game/ArchvisProject/Sequences/SunSky/Seq_MasterSunSky.Seq_MasterSunSky"), TEXT("SunPosition"), TEXT("#7 subsequence track")},
	{TEXT("/Game/ArchvisProject/Sequences/SunSky/Seq_SunSkyEquinox.Seq_SunSkyEquinox"), TEXT("SunPosition"), TEXT("#8 BP-class possessable, integer track")},
	{TEXT("/Game/ArchvisProject/Sequences/SunSky/Seq_SunSkySummerSolstice.Seq_SunSkySummerSolstice"), TEXT("SunPosition"), TEXT("#8")},
	{TEXT("/Game/ArchvisProject/Sequences/SunSky/Seq_SunSkyWinterSolstice.Seq_SunSkyWinterSolstice"), TEXT("SunPosition"), TEXT("#8")},
	{TEXT("/Game/ProductAssets/Sequences/TimeofDaySeq.TimeofDaySeq"), TEXT("SunPosition"), TEXT("#9 dense float curve")},
	{TEXT("/Game/ArchvisProject/Sequences/Stills/LS_Shot_01_626.LS_Shot_01_626"), nullptr, TEXT("#10 still: camera possessable, focal length")},
	{TEXT("/Game/ArchvisProject/Sequences/Stills/LS_Shot_02_773.LS_Shot_02_773"), nullptr, TEXT("#10")},
	{TEXT("/Game/ArchvisProject/Sequences/Stills/LS_Shot_03_316.LS_Shot_03_316"), nullptr, TEXT("#10")},
	{TEXT("/MovieRenderPipeline/Editor/Stills/Sequencer/LS_StillBlank.LS_StillBlank"), TEXT("MovieRenderPipeline"), TEXT("#11 legacy spawnable converted on load")},
	{TEXT("/Game/DMXTemplate/Sequences/LS_FixtureDemo.LS_FixtureDemo"), TEXT("DMXEngine"), TEXT("#12 plugin track and channels, node groups")},
	{TEXT("/Game/DMXTemplate/Sequences/LS_WaterFountain.LS_WaterFountain"), TEXT("DMXEngine"), TEXT("#12")},
};

// The template folders the corpus sequences live in (and what they reference), at their /Game paths.
inline FContentCopies CorpusCopies()
{
	if (PluginEnabled(TEXT("DMXEngine")) && PluginEnabled(TEXT("DMXProtocol")))
	{
		return FContentCopies({{TEXT("Templates/TP_UEIntro_BP/Content"), TEXT("DemoTemplate/Sequencer")},
			{TEXT("Templates/TP_PhotoStudioBP/Content"), TEXT("Studios/Sequences")},
			{TEXT("Templates/TP_AEC_ProdConfigBP/Content"), TEXT("ProductAssets/Sequences")},
			{TEXT("Templates/TP_AEC_ArchvisBP/Content"), TEXT("ArchvisProject/Sequences")},
			{TEXT("Templates/TP_DMXBP/Content"), TEXT("DMXTemplate")}});
	}
	return FContentCopies({{TEXT("Templates/TP_UEIntro_BP/Content"), TEXT("DemoTemplate/Sequencer")},
		{TEXT("Templates/TP_PhotoStudioBP/Content"), TEXT("Studios/Sequences")},
		{TEXT("Templates/TP_AEC_ProdConfigBP/Content"), TEXT("ProductAssets/Sequences")},
		{TEXT("Templates/TP_AEC_ArchvisBP/Content"), TEXT("ArchvisProject/Sequences")}});
}

// The corpus asset when its content is there and its plugin enabled (no load is tried otherwise).
inline ULevelSequence* LoadCorpus(const FCorpusEntry& Entry)
{
	if (Entry.Plugin != nullptr && !PluginEnabled(Entry.Plugin))
	{
		return nullptr;
	}
	const FString Package = FPackageName::ObjectPathToPackageName(FString(Entry.Path));
	if (!FPackageName::IsValidLongPackageName(Package) || !FPackageName::DoesPackageExist(Package))
	{
		return nullptr;
	}
	return Cast<ULevelSequence>(LoadQuiet(Entry.Path));
}

inline FString CorpusName(const FCorpusEntry& Entry)
{
	return FPackageName::ObjectPathToObjectName(FString(Entry.Path));
}

// The editor-built fixture of AssetDocumentSequencerReadTests.cpp (24000 ticks/s at 30 fps).
constexpr int32 TicksPerFrame = 800;
FFrameNumber Frame(int32 DisplayFrame);
TRange<FFrameNumber> Frames(int32 Start, int32 End);
ULevelSequence* NewSequence(const FTarget& Target);
UMovieSceneSection* AddSection(UMovieSceneTrack& Track, int32 Start, int32 End);

struct FBuilt
{
	FGuid Camera;
	FGuid Component;
	FGuid Door;
};

// Spawnable ShotCam (+ CameraComponent child with a dense focal-length channel), possessable Door, folder Cameras,
// transform keys of every interpolation, camera cut, fade, sub track to Inner, marker, metadata; saved.
ULevelSequence* BuildFixture(FAutomationTestBase& Test, const FTarget& Target, const FTarget& InnerTarget, FBuilt& Out);

// JSON navigation.
TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);
TArray<TSharedPtr<FJsonObject>> Items(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);
TSharedPtr<FJsonObject> Named(const TArray<TSharedPtr<FJsonObject>>& List, const TCHAR* Field, const TCHAR* Value);
} // namespace AssetDocumentSequencerTests
#endif
