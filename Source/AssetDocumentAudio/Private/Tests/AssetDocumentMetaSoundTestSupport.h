// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Shared fixtures of the MetaSound automation tests (read: AssetDocumentMetaSoundTests.cpp, apply:
// AssetDocumentMetaSoundApplyTests.cpp): /Game targets, the fidelity corpus, Service calls and comparisons.

#pragma once

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "../../../AssetDocument/Private/Tests/AssetDocumentDiffCountTestSupport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateInput.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMetaSoundTests
{
const TCHAR* const FolderName = TEXT("AssetDocumentMetaSound");
const TCHAR* const SourceClass = TEXT("/Script/MetasoundEngine.MetaSoundSource");
const TCHAR* const OneShot = TEXT("/Metasound/DefaultAssets/Sources/MSS_PlayRandom_Oneshot.MSS_PlayRandom_Oneshot");

inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentMetaSoundTests", "Cleanup", "AssetDocument MetaSound test cleanup"));
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

	// Saved duplicates are unloaded and deleted from disk as the Content Browser does.
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

// The TP_UEIntro_BP template's MetaSounds (and the attenuation MS_TuningFork references) copied into /Game as
// creating a project from the template does; only the files this test copied are removed again.
struct FDemoTemplateContent
{
	TArray<FString> CopiedFiles;
	TArray<FString> CopiedPackages;

	FDemoTemplateContent()
	{
		const FString SourceDir = FPaths::Combine(FPaths::RootDir(), TEXT("Templates/TP_UEIntro_BP/Content/DemoTemplate/Metasounds"));
		const FString TargetDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("DemoTemplate/Metasounds"));
		for (const TCHAR* Name : {TEXT("NewSoundAttenuation"), TEXT("MS_Opera"), TEXT("MS_TuningFork")})
		{
			const FString From = FPaths::Combine(SourceDir, FString(Name) + TEXT(".uasset"));
			const FString To = FPaths::Combine(TargetDir, FString(Name) + TEXT(".uasset"));
			if (!IFileManager::Get().FileExists(*From) || IFileManager::Get().FileExists(*To))
			{
				continue;
			}
			if (IFileManager::Get().Copy(*To, *From) == COPY_OK)
			{
				CopiedFiles.Add(To);
				CopiedPackages.Add(FString(TEXT("/Game/DemoTemplate/Metasounds/")) + Name);
			}
		}
		if (!CopiedFiles.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanFilesSynchronous(CopiedFiles, true);
		}
	}

	~FDemoTemplateContent()
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
		const FString Dir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("DemoTemplate"));
		TArray<FString> Remaining;
		IFileManager::Get().FindFilesRecursive(Remaining, *Dir, TEXT("*"), true, false);
		if (Remaining.IsEmpty())
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

// Duplicates Source into Target's path (the Content Browser's Duplicate) and saves it.
inline UObject* Duplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Copy = AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), Source);
	if (Copy == nullptr)
	{
		return nullptr;
	}
	Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Target.AssetName), UEditorLoadingAndSavingUtils::SavePackages({Copy->GetOutermost()}, false));
	ReleaseUndoHistory();
	return Copy;
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

inline int32 BuilderDocumentCount()
{
	int32 Count = 0;
	for (TObjectIterator<UMetaSoundBuilderDocument> It; It; ++It)
	{
		Count += IsValid(*It) ? 1 : 0;
	}
	return Count;
}

inline bool IsRegistered(const UObject& Asset)
{
	if (const Metasound::Frontend::IMetaSoundAssetManager* Manager = Metasound::Frontend::IMetaSoundAssetManager::Get())
	{
		if (const FMetasoundAssetBase* MetaSound = Manager->GetAsAsset(Asset))
		{
			return MetaSound->IsRegistered();
		}
	}
	return false;
}

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

inline void StripIds(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return;
	}
	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>& Object = Value->AsObject();
		Object->RemoveField(TEXT("Id"));
		for (const auto& Field : Object->Values)
		{
			StripIds(Field.Value);
		}
	}
	else if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
		{
			StripIds(Element);
		}
	}
}

// The document with Target and node / comment ids cleared (the editor gives injected nodes random ids).
inline FString WithoutIds(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Copy = Parse(WithoutTarget(Document));
	if (!Copy.IsValid())
	{
		return FString();
	}
	StripIds(MakeShared<FJsonValueObject>(Copy));
	return Serialize(Copy);
}

// Graph sizes for the report: "Default: 31 nodes, 3 comments".
inline FString GraphSummary(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetObjectField(TEXT("Graphs"), Graphs))
	{
		return TEXT("preset");
	}
	TArray<FString> Parts;
	for (const auto& Page : (*Graphs)->Values)
	{
		const TSharedPtr<FJsonObject> Graph = Page.Value->AsObject();
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		const TSharedPtr<FJsonObject>* Variables = nullptr;
		const TSharedPtr<FJsonObject>* Comments = nullptr;
		Graph->TryGetObjectField(TEXT("Nodes"), Nodes);
		Graph->TryGetObjectField(TEXT("Variables"), Variables);
		Graph->TryGetObjectField(TEXT("Comments"), Comments);
		Parts.Add(FString::Printf(TEXT("%s: %d nodes, %d variables, %d comments"), *Page.Key,
			Nodes != nullptr ? (*Nodes)->Values.Num() : 0, Variables != nullptr ? (*Variables)->Values.Num() : 0,
			Comments != nullptr ? (*Comments)->Values.Num() : 0));
	}
	return FString::Join(Parts, TEXT("; "));
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

struct FCorpusEntry
{
	const TCHAR* Path;
	const TCHAR* Why;
};

const FCorpusEntry Corpus[] = {
	{TEXT("/Metasound/DefaultAssets/Sources/MSS_PlayRandom_Oneshot.MSS_PlayRandom_Oneshot"), TEXT("canonical source")},
	{TEXT("/Metasound/DefaultAssets/Sources/MSS_PlayRandom_Loop.MSS_PlayRandom_Loop"), TEXT("no OneShot")},
	{TEXT("/Game/DemoTemplate/Metasounds/MS_TuningFork.MS_TuningFork"), TEXT("largest, variables")},
	{TEXT("/Game/DemoTemplate/Metasounds/MS_Opera.MS_Opera"), TEXT("mono, enums")},
	{TEXT("/Harmonix/Tests/HarmonixMetasound/FusionSamplerNode/MS_Test_FusionSampler_SynSamples.MS_Test_FusionSampler_SynSamples"), TEXT("asset reference")},
	{TEXT("/Harmonix/MetaSounds/MSP_MidiNoteAdsr.MSP_MidiNoteAdsr"), TEXT("legacy patch")},
	{TEXT("/Harmonix/Tests/HarmonixMetasound/MS_MIDIStreamSineGenerator.MS_MIDIStreamSineGenerator"), TEXT("patch")},
	{TEXT("/Harmonix/Tests/HarmonixMetasound/MS_TriggerPulseSineGenerator.MS_TriggerPulseSineGenerator"), TEXT("patch, triggers")},
	{TEXT("/Harmonix/Tests/HarmonixMetasound/MidiPlayerNode/MS_Test_MidiPlayerNode_Simple.MS_Test_MidiPlayerNode_Simple"), TEXT("legacy, patch node")},
	{TEXT("/Harmonix/Tests/HarmonixMetasound/MidiPlayerNode/MS_Test_MidiPlayerNode_Metronome_MidiClockTriggers.MS_Test_MidiPlayerNode_Metronome_MidiClockTriggers"), TEXT("legacy, 42 nodes")},
	{TEXT("/Harmonix/Tests/HarmonixMetasound/MidiPlayerNode/MS_Test_MidiPlayerNode_Metronome_AuthoredTimeSigChanges.MS_Test_MidiPlayerNode_Metronome_AuthoredTimeSigChanges"), TEXT("legacy comment")},
	{TEXT("/Harmonix/Tests/HarmonixMetasound/StepSequenceNode/MS_Test_StepSequencePlayer_PageChanges.MS_Test_StepSequencePlayer_PageChanges"), TEXT("legacy, 49 nodes")},
	{TEXT("/Harmonix/Examples/Sources/MSS_MIDISequencePlayer.MSS_MIDISequencePlayer"), TEXT("UE.Source triggers")},
	{TEXT("/Harmonix/Tests/HarmonixMetasoundMusicClockComponent/MSS_MetronomeClockGenerator.MSS_MetronomeClockGenerator"), TEXT("interface override")},
	{TEXT("/Harmonix/Tests/HarmonixMetasound/MidiPlayerNode/MS_Test_MidiPlayerNode_Metronome_TempoChanges.MS_Test_MidiPlayerNode_Metronome_TempoChanges"), TEXT("Trigger Route")},
};

inline UObject* LoadQuiet(const FString& Path)
{
	return LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// Reading leaves no dirty package, no builder document (after GC) and the registration state unchanged.
struct FReadSideEffects
{
	TSet<FString> Dirty;
	int32 Builders = 0;
	bool bRegistered = false;

	explicit FReadSideEffects(const UObject& Asset)
		: Dirty(DirtyPackages())
		, Builders((CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS), BuilderDocumentCount()))
		, bRegistered(IsRegistered(Asset))
	{
	}

	void Check(const UObject& Asset, TArray<FString>& OutProblems) const
	{
		const TSet<FString> After = DirtyPackages();
		for (const FString& Name : After)
		{
			if (!Dirty.Contains(Name))
			{
				OutProblems.Add(FString::Printf(TEXT("reading dirtied %s"), *Name));
			}
		}
		if (IsRegistered(Asset) != bRegistered)
		{
			OutProblems.Add(FString::Printf(TEXT("reading changed the registration (%d -> %d)"), bRegistered ? 1 : 0, bRegistered ? 0 : 1));
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		const int32 BuildersAfter = BuilderDocumentCount();
		if (BuildersAfter != Builders)
		{
			OutProblems.Add(FString::Printf(TEXT("reading left builder documents (%d -> %d)"), Builders, BuildersAfter));
		}
	}
};

// Turns the asset back into what a MetaSound saved before input template nodes looked like (and what the
// editor injects on open): every input template node is removed and its consumers are fed by the input node
// directly. Returns the number of template nodes removed.
inline int32 RemoveInputTemplates(UObject& Asset)
{
	FMetaSoundFrontendDocumentBuilder Builder{TScriptInterface<IMetaSoundDocumentInterface>(&Asset)};
	struct FRewire
	{
		FGuid TemplateID;
		FGuid InputNodeID;
		FGuid InputVertexID;
		TArray<FMetasoundFrontendEdge> Consumers;
	};
	TArray<FRewire> Rewires;
	const FMetasoundFrontendGraph& Graph = Builder.FindConstBuildGraphChecked();
	for (const FMetasoundFrontendNode& Node : Graph.Nodes)
	{
		const FMetasoundFrontendClass* Class = Builder.FindDependency(Node.ClassID);
		if (Class == nullptr || Class->Metadata.GetClassName() != Metasound::Frontend::FInputNodeTemplate::ClassName)
		{
			continue;
		}
		FRewire Rewire;
		Rewire.TemplateID = Node.GetID();
		for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
		{
			if (Edge.ToNodeID == Node.GetID())
			{
				Rewire.InputNodeID = Edge.FromNodeID;
				Rewire.InputVertexID = Edge.FromVertexID;
			}
			else if (Edge.FromNodeID == Node.GetID())
			{
				Rewire.Consumers.Add(Edge);
			}
		}
		if (Rewire.InputNodeID.IsValid() && !Rewire.Consumers.IsEmpty())
		{
			Rewires.Add(MoveTemp(Rewire));
		}
	}
	for (FRewire& Rewire : Rewires)
	{
		Builder.RemoveNode(Rewire.TemplateID);
		for (FMetasoundFrontendEdge& Edge : Rewire.Consumers)
		{
			Edge.FromNodeID = Rewire.InputNodeID;
			Edge.FromVertexID = Rewire.InputVertexID;
			Builder.AddEdge(MoveTemp(Edge));
		}
	}
	return Rewires.Num();
}
} // namespace AssetDocumentMetaSoundTests
#endif // WITH_DEV_AUTOMATION_TESTS
