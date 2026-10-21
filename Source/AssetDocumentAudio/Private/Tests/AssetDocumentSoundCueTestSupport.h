// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Shared fixtures of the SoundCue automation tests (AssetDocumentSoundCueTests.cpp: read, corpus, editor delete,
// renamed nodes; AssetDocumentSoundCueApplyTests.cpp: free edits, from scratch, rollback, undo, open editor): /Game
// targets, the corpus, the editor-side operations (the Sound Cue editor's own calls), the editor-built fixtures and
// the comparison form of a cue (graph, runtime tree, derived values, status).
//
// No audio device is needed: nothing here plays a sound.

#pragma once

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "../../../AssetDocument/Private/Tests/AssetDocumentDiffCountTestSupport.h"

#include "AssetDocumentGraphPropertyCodec.h"
#include "AssetDocumentGraphValue.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Factories/SoundCueFactoryNew.h"
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
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/AudioSettings.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeEnveloper.h"
#include "Sound/SoundWave.h"
#include "SoundCue/AssetDocumentSoundCue.h"
#include "SoundCueGraph/SoundCueGraph.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentSoundCueTests
{
using namespace UE::AssetDocument::Graph;
namespace SC = UE::AssetDocument::Graph::Audio::SoundCue;

const TCHAR* const FolderName = TEXT("AssetDocumentSoundCue");
const TCHAR* const CueClass = TEXT("/Script/Engine.SoundCue");
const TCHAR* const WaveA = TEXT("/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess");
const TCHAR* const WaveB = TEXT("/Engine/EditorSounds/Notifications/CompileFailed.CompileFailed");
const TCHAR* const WaveC = TEXT("/Engine/VREditor/Sounds/UI/Click_on_Button.Click_on_Button");
const TCHAR* const WaveD = TEXT("/Engine/VREditor/Sounds/UI/Camera_Shutter.Camera_Shutter");

inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentSoundCueTests", "Cleanup", "AssetDocument SoundCue test cleanup"));
	}
}

inline UObject* LoadQuiet(const FString& Path)
{
	// Content of an unmounted optional plugin: no load attempt (LoadPackage would log a warning).
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	if (!FPackageName::IsValidPath(PackageName) || !FPackageName::DoesPackageExist(PackageName))
	{
		return nullptr;
	}
	return LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

inline double Milliseconds(double Start)
{
	return (FPlatformTime::Seconds() - Start) * 1000.0;
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
	USoundCue* Cue() const { return Cast<USoundCue>(Get()); }

	// Saved assets are unloaded and deleted from disk as the Content Browser does.
	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
		{
			if (GEditor != nullptr)
			{
				if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					Editors->CloseAllEditorsForAsset(Asset);
				}
			}
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

// The XRFramework template's Fire_Cue (and its wave) copied into /Game as creating a project from the template does;
// only the files this test copied are removed again.
struct FTemplateContent
{
	TArray<FString> CopiedFiles;
	TArray<FString> CopiedPackages;

	FTemplateContent()
	{
		const FString SourceDir = FPaths::Combine(FPaths::RootDir(), TEXT("Templates/TemplateResources/Standard/XRFramework/Content/Audio"));
		const FString TargetDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("XRFramework/Audio"));
		for (const TCHAR* Name : {TEXT("Fire01"), TEXT("Fire_Cue")})
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
				CopiedPackages.Add(FString(TEXT("/Game/XRFramework/Audio/")) + Name);
			}
		}
		if (!CopiedFiles.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanFilesSynchronous(CopiedFiles, true);
		}
	}

	~FTemplateContent()
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
		const FString Dir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("XRFramework"));
		TArray<FString> Remaining;
		IFileManager::Get().FindFilesRecursive(Remaining, *Dir, TEXT("*"), true, false);
		if (Remaining.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
		}
	}
};

// ---- JSON and Service ---------------------------------------------------------------------------------------------

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

inline TSharedPtr<FJsonObject> Json(const TCHAR* Text)
{
	return Parse(Text);
}

inline TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Parent, const FString& Name)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	if (Parent.IsValid() && Parent->TryGetObjectField(Name, Found))
	{
		return *Found;
	}
	return MakeShared<FJsonObject>();
}

inline TSharedPtr<FJsonObject> BodyOf(const TSharedPtr<FJsonObject>& Document) { return Child(Document, TEXT("Body")); }
inline TSharedPtr<FJsonObject> NodesOf(const TSharedPtr<FJsonObject>& Document) { return Child(BodyOf(Document), TEXT("Nodes")); }
inline TSharedPtr<FJsonObject> NodeOf(const TSharedPtr<FJsonObject>& Document, const FString& Key) { return Child(NodesOf(Document), Key); }
inline TSharedPtr<FJsonObject> CommentsOf(const TSharedPtr<FJsonObject>& Document) { return Child(BodyOf(Document), TEXT("Comments")); }

inline TArray<TSharedPtr<FJsonValue>> Keys(std::initializer_list<const TCHAR*> Items)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const TCHAR* Item : Items)
	{
		Out.Add(Item != nullptr ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Item)) : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
	}
	return Out;
}

inline TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Items)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const double Item : Items)
	{
		Out.Add(MakeShared<FJsonValueNumber>(Item));
	}
	return Out;
}

// A copy of Document for Target with Action.
inline TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	if (Copy.IsValid())
	{
		Copy->SetStringField(TEXT("Target"), Target.PackageName);
		Copy->SetStringField(TEXT("Action"), Action);
	}
	return Copy;
}

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
	return FString::Printf(TEXT("at %d: '%s' vs '%s'"), Index, *A.Mid(Start, 220), *B.Mid(Start, 220));
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

inline FString StatusOf(const FAssetDocumentResult& Result)
{
	if (!Result.Compile.IsValid())
	{
		return TEXT("-");
	}
	TArray<FString> Parts = {Result.Compile->GetStringField(TEXT("Status"))};
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (Result.Compile->TryGetArrayField(TEXT("Messages"), Messages))
	{
		for (const TSharedPtr<FJsonValue>& Message : *Messages)
		{
			Parts.Add(Message->AsObject()->GetStringField(TEXT("Severity")) + TEXT(": ") + Message->AsObject()->GetStringField(TEXT("Message")));
		}
	}
	return FString::Join(Parts, TEXT(" | "));
}

inline bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess()))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return false;
	}
	return true;
}

// A refused apply: its code and path, and an unchanged asset (package clean, file untouched, same extraction).
inline void ExpectRefused(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code,
	const FString& Path, const FString& Stage)
{
	const FString Before = Serialize(Extract(Test, Target.ObjectPath, Stage + TEXT(" before")));
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	const FAssetDocumentResult Result = Apply(Target, Document);
	Test.TestFalse(*FString::Printf(TEXT("%s: refused"), *Stage), Result.IsSuccess());
	const FAssetDocumentDiagnostic* Found = Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s"), *Stage, Code), Found))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
	}
	else
	{
		Test.TestEqual(*FString::Printf(TEXT("%s: path"), *Stage), Found->Path, Path);
	}
	const UObject* Asset = Target.Get();
	Test.TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Asset != nullptr && Asset->GetOutermost()->IsDirty());
	Test.TestEqual(*FString::Printf(TEXT("%s: package file untouched"), *Stage), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
	Test.TestEqual(*FString::Printf(TEXT("%s: extraction unchanged"), *Stage), Serialize(Extract(Test, Target.ObjectPath, Stage + TEXT(" after"))), Before);
}

inline bool Save(FAutomationTestBase& Test, UObject* Asset, const FString& Stage)
{
	return Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Stage), Asset != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Asset->GetOutermost()}, false));
}

// Duplicates Source into Target's path (the Content Browser's Duplicate) and saves it.
inline USoundCue* Duplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Copy = Source != nullptr ? AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), Source) : nullptr;
	if (Copy == nullptr)
	{
		return nullptr;
	}
	Save(Test, Copy, Target.AssetName);
	ReleaseUndoHistory();
	return Cast<USoundCue>(Copy);
}

// The Content Browser's Add > Sounds > Sound Cue (USoundCueFactoryNew), with InitialSoundWaves when given (the
// Content Browser's "Create Cue" on selected waves). Saved.
inline USoundCue* CreateCue(FAutomationTestBase& Test, const FTarget& Target, TConstArrayView<const TCHAR*> InitialWaves = {})
{
	USoundCueFactoryNew* Factory = NewObject<USoundCueFactoryNew>();
	for (const TCHAR* Path : InitialWaves)
	{
		Factory->InitialSoundWaves.Add(Cast<USoundWave>(LoadQuiet(Path)));
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	USoundCue* Cue = Cast<USoundCue>(AssetTools.CreateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), USoundCue::StaticClass(), Factory));
	if (Test.TestNotNull(*FString::Printf(TEXT("%s created by the factory"), *Target.AssetName), Cue))
	{
		Save(Test, Cue, Target.AssetName);
	}
	ReleaseUndoHistory();
	return Cue;
}

// ---- the editor side: the Sound Cue editor's own calls --------------------------------------------------------------

struct FEditorCue
{
	USoundCue& Cue;

	explicit FEditorCue(USoundCue& InCue) : Cue(InCue) {}

	// Editor-side steps report a step that did not happen to the running test.
	static bool Expect(const TCHAR* What, bool bValue)
	{
		FAutomationTestBase* Current = FAutomationTestFramework::Get().GetCurrentTest();
		return Current != nullptr ? Current->TestTrue(What, bValue) : bValue;
	}

	UEdGraph& Graph() const { return *Cue.SoundCueGraph; }
	const UEdGraphSchema& Schema() const { return *Cue.SoundCueGraph->GetSchema(); }

	USoundCueGraphNode_Root* Root() const
	{
		for (UEdGraphNode* Node : Cue.SoundCueGraph->Nodes)
		{
			if (USoundCueGraphNode_Root* Found = Cast<USoundCueGraphNode_Root>(Node))
			{
				return Found;
			}
		}
		return nullptr;
	}

	// The graph node whose identity (sound node or comment object name) is Key.
	UEdGraphNode* Find(const FString& Key) const
	{
		for (UEdGraphNode* Node : Cue.SoundCueGraph->Nodes)
		{
			if (Node != nullptr && SC::NodeIdentity(*Node) == Key)
			{
				return Node;
			}
		}
		return nullptr;
	}

	USoundCueGraphNode* Sound(const FString& Key) const { return Cast<USoundCueGraphNode>(Find(Key)); }

	// The palette: drop a node of the class at (X, Y). Returns the new node's key.
	FString Add(const TCHAR* ClassLabel, int32 X, int32 Y) const
	{
		UClass* Class = SC::ResolveClassLabel(ClassLabel);
		UEdGraphNode* Node = Class != nullptr ? SC::PerformPaletteAction(Graph(), Class, FVector2f(static_cast<float>(X), static_cast<float>(Y))) : nullptr;
		return Node != nullptr ? SC::NodeIdentity(*Node) : FString();
	}

	FString AddComment(const TCHAR* Text, int32 X, int32 Y, int32 Width, int32 Height) const
	{
		UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(SC::PerformPaletteAction(Graph(), nullptr, FVector2f(static_cast<float>(X), static_cast<float>(Y))));
		if (Comment == nullptr)
		{
			return FString();
		}
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCueTests", "EditComment", "Edit Comment"));
		Comment->Modify();
		Comment->NodeComment = Text;
		Comment->ResizeNode(FVector2f(static_cast<float>(Width), static_cast<float>(Height)));
		return Comment->GetName();
	}

	// Dragging a wire from Child's output to input pin Slot of Parent ("" = the Output node).
	bool Connect(const FString& Child, const FString& Parent, int32 Slot) const
	{
		USoundCueGraphNode* From = Sound(Child);
		UEdGraphPin* To = Parent.IsEmpty() ? (Root() != nullptr ? Root()->GetInputPin(0) : nullptr)
			: (Sound(Parent) != nullptr ? Sound(Parent)->GetInputPin(Slot) : nullptr);
		if (From == nullptr || To == nullptr)
		{
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCueTests", "LinkPins", "Link Pins"));
		return Schema().TryCreateConnection(From->GetOutputPin(), To);
	}

	// Alt-click (Break Link) on input pin Slot of Parent ("" = the Output node).
	void Break(const FString& Parent, int32 Slot) const
	{
		UEdGraphPin* Pin = Parent.IsEmpty() ? Root()->GetInputPin(0) : Sound(Parent)->GetInputPin(Slot);
		Schema().BreakPinLinks(*Pin, true);
	}

	// Node context menu: Add Input / Delete Input.
	void AddInput(const FString& Key) const { Sound(Key)->AddInputPin(); }
	void RemoveInput(const FString& Key, int32 Slot) const { Sound(Key)->RemoveInputPin(Sound(Key)->GetInputPin(Slot)); }

	// The editor's Delete (the replica; UEAssetDoc.SoundCue.EditorDelete compares it with the editor).
	bool Delete(const FString& Key) const
	{
		FString Error;
		return Find(Key) != nullptr && SC::DeleteNodeAsTheEditorDoes(Cue, *Find(Key), Error);
	}

	// A Details panel edit of the node's sound node (or a comment's own Details): value as JSON text.
	bool SetDetail(const FString& Key, const TCHAR* Name, const FString& JsonValue) const
	{
		UEdGraphNode* Node = Find(Key);
		UObject* Target = Node == nullptr ? nullptr : Node->IsA<USoundCueGraphNode>() ? static_cast<UObject*>(Cast<USoundCueGraphNode>(Node)->SoundNode.Get()) : static_cast<UObject*>(Node);
		FProperty* Property = Target != nullptr ? Target->GetClass()->FindPropertyByName(Name) : nullptr;
		// Wrapped in an object: the JSON reader takes only an object or an array at the top level.
		FValue Wrapped;
		const FValue* Value = TryParseJsonValue(FString::Printf(TEXT("{\"v\": %s}"), *JsonValue), Wrapped) ? FindField(Wrapped, TEXT("v")) : nullptr;
		if (!Expect(*FString::Printf(TEXT("Details edit %s.%s: property and value"), *Key, Name), Property != nullptr && Value != nullptr))
		{
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCueTests", "EditDetail", "Edit Property"));
		Target->Modify();
		Target->PreEditChange(Property);
		FString Error;
		const bool bDecoded = DecodePropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(Target), *Value, Error);
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Target->PostEditChangeProperty(Event);
		return Expect(*FString::Printf(TEXT("Details edit %s.%s: %s"), *Key, Name, *Error), bDecoded);
	}

	// Dragging a node / typing its comment bubble.
	void Move(const FString& Key, int32 X, int32 Y) const
	{
		UEdGraphNode* Node = Key.IsEmpty() ? static_cast<UEdGraphNode*>(Root()) : Find(Key);
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCueTests", "MoveNode", "Move Node"));
		Node->Modify();
		Node->NodePosX = X;
		Node->NodePosY = Y;
	}

	void SetBubble(const FString& Key, const TCHAR* Text) const
	{
		UEdGraphNode* Node = Find(Key);
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCueTests", "EditComment", "Edit Comment"));
		Node->Modify();
		Node->NodeComment = Text;
		Node->bCommentBubbleVisible = true;
		Node->bCommentBubblePinned = true;
	}
};

// ---- editor-built fixtures (design §6.1) ----------------------------------------------------------------------------

// SC_AllNodes: one node of every engine class in a DAG (a wave player shared by two parents), Switch with 5 slots,
// Branch, Group Control with 3 groups, Concatenator, crossfades with non-default data, Enveloper with curves,
// Continuous Modulator, Attenuation override, Sound Class, Wave Param, Mature, Looping (3), Delay, Oscillator,
// Doppler, comments, comment bubbles, an orphan node.
inline bool BuildAllNodes(FAutomationTestBase& Test, USoundCue& Cue)
{
	const FEditorCue E(Cue);
	TMap<FString, FString> K;
	int32 Y = -1200;
	for (const TCHAR* Label : {TEXT("Mixer"), TEXT("Switch"), TEXT("Branch"), TEXT("GroupControl"), TEXT("Concatenator"),
		TEXT("DistanceCrossFade"), TEXT("ParamCrossFade"), TEXT("Enveloper"), TEXT("ModulatorContinuous"), TEXT("Attenuation"),
		TEXT("SoundClass"), TEXT("WaveParam"), TEXT("Mature"), TEXT("Looping"), TEXT("Delay"), TEXT("Oscillator"), TEXT("Doppler"),
		TEXT("Modulator"), TEXT("Random"), TEXT("DialoguePlayer")})
	{
		K.Add(Label, E.Add(Label, -400, Y));
		Y += 120;
		if (!Test.TestFalse(*FString::Printf(TEXT("palette places %s"), Label), K[Label].IsEmpty()))
		{
			return false;
		}
	}
	TArray<FString> Players;
	const TCHAR* Waves[] = {WaveA, WaveB, WaveC, WaveD};
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const FString Key = E.Add(TEXT("WavePlayer"), -900, -1200 + Index * 140);
		E.SetDetail(Key, TEXT("SoundWaveAssetPtr"), FString::Printf(TEXT("\"%s\""), Waves[Index % 4]));
		Players.Add(Key);
	}
	E.SetDetail(Players[1], TEXT("bLooping"), TEXT("true"));
	// Root <- Mixer (4 inputs).
	E.Connect(K[TEXT("Mixer")], TEXT(""), 0);
	E.AddInput(K[TEXT("Mixer")]);
	E.AddInput(K[TEXT("Mixer")]);
	E.Connect(K[TEXT("Switch")], K[TEXT("Mixer")], 0);
	E.Connect(K[TEXT("Branch")], K[TEXT("Mixer")], 1);
	E.Connect(K[TEXT("Concatenator")], K[TEXT("Mixer")], 2);
	E.Connect(K[TEXT("Attenuation")], K[TEXT("Mixer")], 3);
	E.SetDetail(K[TEXT("Mixer")], TEXT("InputVolume"), TEXT("[1.0, 0.8, 0.6, 0.4]"));
	// Switch: 4 starting slots + 1.
	E.AddInput(K[TEXT("Switch")]);
	E.SetDetail(K[TEXT("Switch")], TEXT("IntParameterName"), TEXT("\"Surface\""));
	E.Connect(K[TEXT("GroupControl")], K[TEXT("Switch")], 1);
	E.Connect(K[TEXT("Random")], K[TEXT("Switch")], 2);
	E.Connect(Players[0], K[TEXT("Switch")], 4);
	// Group Control with 3 groups.
	E.AddInput(K[TEXT("GroupControl")]);
	E.Connect(Players[1], K[TEXT("GroupControl")], 0);
	E.Connect(Players[2], K[TEXT("GroupControl")], 1);
	E.Connect(Players[3], K[TEXT("GroupControl")], 2);
	E.SetDetail(K[TEXT("GroupControl")], TEXT("GroupSizes"), TEXT("[2, 3]"));
	// Random over Modulator / Continuous Modulator, weights.
	E.Connect(K[TEXT("Modulator")], K[TEXT("Random")], 0);
	E.Connect(K[TEXT("ModulatorContinuous")], K[TEXT("Random")], 1);
	E.SetDetail(K[TEXT("Random")], TEXT("Weights"), TEXT("[2.0, 0.5]"));
	E.SetDetail(K[TEXT("Random")], TEXT("bRandomizeWithoutReplacement"), TEXT("false"));
	E.Connect(Players[4], K[TEXT("Modulator")], 0);
	E.SetDetail(K[TEXT("Modulator")], TEXT("PitchMin"), TEXT("0.8"));
	E.Connect(Players[4], K[TEXT("ModulatorContinuous")], 0);  // a player shared by two parents
	// Branch: True <- Mature <- (Looping, Delay); False <- Enveloper; Unset <- Oscillator.
	E.SetDetail(K[TEXT("Branch")], TEXT("BoolParameterName"), TEXT("\"Wet\""));
	E.Connect(K[TEXT("Mature")], K[TEXT("Branch")], 0);
	E.Connect(K[TEXT("Enveloper")], K[TEXT("Branch")], 1);
	E.Connect(K[TEXT("Oscillator")], K[TEXT("Branch")], 2);
	E.Connect(K[TEXT("Looping")], K[TEXT("Mature")], 0);
	E.Connect(K[TEXT("Delay")], K[TEXT("Mature")], 1);
	E.SetDetail(K[TEXT("Looping")], TEXT("LoopCount"), TEXT("3"));
	E.SetDetail(K[TEXT("Looping")], TEXT("bLoopIndefinitely"), TEXT("false"));
	E.Connect(Players[5], K[TEXT("Looping")], 0);
	E.SetDetail(K[TEXT("Delay")], TEXT("DelayMax"), TEXT("0.25"));
	E.Connect(K[TEXT("WaveParam")], K[TEXT("Delay")], 0);
	E.SetDetail(K[TEXT("WaveParam")], TEXT("WaveParameterName"), TEXT("\"Override\""));
	E.Connect(Players[6], K[TEXT("WaveParam")], 0);
	E.Connect(K[TEXT("Doppler")], K[TEXT("Enveloper")], 0);
	E.SetDetail(K[TEXT("Doppler")], TEXT("DopplerIntensity"), TEXT("2.5"));
	E.Connect(Players[7], K[TEXT("Doppler")], 0);
	E.SetDetail(K[TEXT("Enveloper")], TEXT("LoopStart"), TEXT("0.1"));
	if (USoundNodeEnveloper* Enveloper = Cast<USoundNodeEnveloper>(E.Sound(K[TEXT("Enveloper")])->SoundNode))
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCueTests", "EditCurve", "Edit Curve"));
		Enveloper->Modify();
		Enveloper->VolumeCurve.GetRichCurve()->AddKey(0.0f, 0.2f);
		Enveloper->VolumeCurve.GetRichCurve()->AddKey(1.0f, 1.0f);
		Enveloper->PitchCurve.GetRichCurve()->AddKey(0.5f, 1.2f);
		Enveloper->PostEditChange();
	}
	E.Connect(Players[0], K[TEXT("Oscillator")], 0);
	E.SetDetail(K[TEXT("Oscillator")], TEXT("FrequencyMax"), TEXT("3.0"));
	// Concatenator: Sound Class, Distance crossfade; Attenuation <- Param crossfade.
	E.Connect(K[TEXT("SoundClass")], K[TEXT("Concatenator")], 0);
	E.Connect(K[TEXT("DistanceCrossFade")], K[TEXT("Concatenator")], 1);
	E.SetDetail(K[TEXT("Concatenator")], TEXT("InputVolume"), TEXT("[0.9, 0.7]"));
	E.Connect(Players[2], K[TEXT("SoundClass")], 0);
	E.SetDetail(K[TEXT("SoundClass")], TEXT("SoundClassOverride"), TEXT("\"/Engine/EngineSounds/Master.Master\""));
	E.Connect(Players[3], K[TEXT("DistanceCrossFade")], 0);
	E.Connect(Players[5], K[TEXT("DistanceCrossFade")], 1);
	E.SetDetail(K[TEXT("DistanceCrossFade")], TEXT("CrossFadeInput"),
		TEXT("[{\"FadeInDistanceStart\": 0, \"FadeInDistanceEnd\": 0, \"FadeOutDistanceStart\": 400, \"FadeOutDistanceEnd\": 600, \"Volume\": 1.0},"
			 " {\"FadeInDistanceStart\": 400, \"FadeInDistanceEnd\": 600, \"FadeOutDistanceStart\": 2000, \"FadeOutDistanceEnd\": 2500, \"Volume\": 0.7}]"));
	E.Connect(K[TEXT("ParamCrossFade")], K[TEXT("Attenuation")], 0);
	E.SetDetail(K[TEXT("ParamCrossFade")], TEXT("ParamName"), TEXT("\"Distance\""));
	E.Connect(Players[6], K[TEXT("ParamCrossFade")], 0);
	E.Connect(Players[7], K[TEXT("ParamCrossFade")], 1);
	E.SetDetail(K[TEXT("Attenuation")], TEXT("bOverrideAttenuation"), TEXT("true"));
	// DialoguePlayer stays an orphan (no dialogue wave).
	E.Move(K[TEXT("DialoguePlayer")], -400, 1400);
	E.SetBubble(Players[0], TEXT("shared variation"));
	E.SetBubble(K[TEXT("Switch")], TEXT("per surface"));
	E.AddComment(TEXT("Players"), -950, -1300, 420, 1300);
	const FString Second = E.AddComment(TEXT("Logic"), -450, -1300, 300, 2600);
	E.SetDetail(Second, TEXT("FontSize"), TEXT("24"));
	E.SetDetail(Second, TEXT("CommentColor"), TEXT("{\"R\": 0.2, \"G\": 0.6, \"B\": 1.0, \"A\": 1.0}"));
	return true;
}

// The project's quality levels (UAudioSettings::QualityLevels, reflected: the member is not public), set and restored.
struct FQualityLevels
{
	TArray<FAudioQualitySettings>* Levels = nullptr;
	TArray<FAudioQualitySettings> Saved;

	explicit FQualityLevels(TConstArrayView<const TCHAR*> Names)
	{
		UAudioSettings* Settings = GetMutableDefault<UAudioSettings>();
		const FArrayProperty* Property = CastField<FArrayProperty>(UAudioSettings::StaticClass()->FindPropertyByName(TEXT("QualityLevels")));
		if (Property == nullptr)
		{
			return;
		}
		Levels = Property->ContainerPtrToValuePtr<TArray<FAudioQualitySettings>>(Settings);
		Saved = *Levels;
		Levels->Reset();
		for (const TCHAR* Name : Names)
		{
			FAudioQualitySettings& Level = Levels->AddDefaulted_GetRef();
			Level.DisplayName = FText::FromString(Name);
		}
	}

	~FQualityLevels()
	{
		if (Levels != nullptr)
		{
			*Levels = Saved;
		}
	}
};

// SC_Quality: a Quality Level node (one input per project quality level) over two players.
inline bool BuildQuality(FAutomationTestBase& Test, USoundCue& Cue)
{
	const FEditorCue E(Cue);
	const FString Quality = E.Add(TEXT("QualityLevel"), -300, 0);
	const FString High = E.Add(TEXT("WavePlayer"), -700, -100);
	const FString Low = E.Add(TEXT("WavePlayer"), -700, 100);
	E.SetDetail(High, TEXT("SoundWaveAssetPtr"), FString::Printf(TEXT("\"%s\""), WaveA));
	E.SetDetail(Low, TEXT("SoundWaveAssetPtr"), FString::Printf(TEXT("\"%s\""), WaveB));
	E.Connect(Quality, TEXT(""), 0);
	E.Connect(High, Quality, 0);
	E.Connect(Low, Quality, 1);
	return Test.TestEqual(TEXT("Quality Level has one input per quality level"), E.Sound(Quality)->GetInputCount(), GetDefault<UAudioSettings>()->GetQualityLevelSettingsNum());
}

// ---- comparison form -------------------------------------------------------------------------------------------------

inline FString Reflected(const UObject& Object, const TCHAR* Name)
{
	const FProperty* Property = Object.GetClass()->FindPropertyByName(Name);
	if (Property == nullptr)
	{
		return TEXT("-");
	}
	FString Text;
	Property->ExportTextItem_Direct(Text, Property->ContainerPtrToValuePtr<void>(&Object), nullptr, nullptr, PPF_None);
	return Text;
}

inline FString PinLink(const UEdGraphPin* Pin)
{
	if (Pin == nullptr || Pin->LinkedTo.IsEmpty() || Pin->LinkedTo[0] == nullptr)
	{
		return TEXT("-");
	}
	const UEdGraphNode* Owner = Pin->LinkedTo[0]->GetOwningNode();
	return Owner->IsA<USoundCueGraphNode_Root>() ? FString(TEXT("Root")) : SC::NodeIdentity(*Owner);
}

// The cue as the editor holds it: the graph (nodes by key: class, position, bubble, pins and their links, Details),
// comments, the runtime tree (FirstNode, AllNodes, ChildNodes), the derived values and the validation status.
inline FString CueForm(const USoundCue& Cue, bool bPositions = true)
{
	TArray<FString> Lines;
	for (const UEdGraphNode* Node : Cue.SoundCueGraph->Nodes)
	{
		if (Node == nullptr)
		{
			continue;
		}
		const FString Where = bPositions ? FString::Printf(TEXT(" @%d,%d"), Node->NodePosX, Node->NodePosY) : FString();
		if (const USoundCueGraphNode_Root* Root = Cast<USoundCueGraphNode_Root>(Node))
		{
			TArray<UEdGraphPin*> Inputs;
			const_cast<USoundCueGraphNode_Root*>(Root)->GetInputPins(Inputs);
			Lines.Add(FString::Printf(TEXT("root%s <- %s"), *Where, Inputs.IsEmpty() ? TEXT("-") : *PinLink(Inputs[0])));
		}
		else if (const USoundCueGraphNode* Sound = Cast<USoundCueGraphNode>(Node))
		{
			TArray<UEdGraphPin*> Inputs;
			const_cast<USoundCueGraphNode*>(Sound)->GetInputPins(Inputs);
			TArray<FString> Pins;
			for (const UEdGraphPin* Pin : Inputs)
			{
				Pins.Add(FString::Printf(TEXT("%s<-%s"), *Pin->PinName.ToString(), *PinLink(Pin)));
			}
			Lines.Add(FString::Printf(TEXT("node %s %s%s bubble='%s'%d [%s] %s"), *SC::NodeIdentity(*Node),
				Sound->SoundNode != nullptr ? *Sound->SoundNode->GetClass()->GetPathName() : TEXT("null"), *Where, *Node->NodeComment,
				Node->bCommentBubbleVisible ? 1 : 0, *FString::Join(Pins, TEXT(", ")),
				Sound->SoundNode != nullptr ? *ValueToJsonString(SC::EncodeChangedProperties(*Sound->SoundNode), false) : TEXT("")));
		}
		else
		{
			Lines.Add(FString::Printf(TEXT("comment %s '%s'%s size=%d,%d %s"), *Node->GetName(), *Node->NodeComment, *Where, Node->NodeWidth,
				Node->NodeHeight, *ValueToJsonString(SC::EncodeChangedCommentProperties(*Node), false)));
		}
	}
	Lines.Sort();
	TArray<FString> Runtime;
	for (const USoundNode* Node : Cue.AllNodes)
	{
		TArray<FString> Children;
		if (Node != nullptr)
		{
			for (const USoundNode* ChildNode : Node->ChildNodes)
			{
				Children.Add(ChildNode != nullptr ? ChildNode->GetName() : FString(TEXT("-")));
			}
		}
		Runtime.Add(FString::Printf(TEXT("runtime %s -> [%s]"), Node != nullptr ? *Node->GetName() : TEXT("null"), *FString::Join(Children, TEXT(", "))));
	}
	Runtime.Sort();
	Lines.Append(Runtime);
	Lines.Add(FString::Printf(TEXT("first %s"), Cue.FirstNode != nullptr ? *Cue.FirstNode->GetName() : TEXT("-")));
	Lines.Add(FString::Printf(TEXT("derived Duration=%s MaxDistance=%s Delay=%s Concatenator=%s PlayWhenSilent=%s"), *Reflected(Cue, TEXT("Duration")),
		*Reflected(Cue, TEXT("MaxDistance")), *Reflected(Cue, TEXT("bHasDelayNode")), *Reflected(Cue, TEXT("bHasConcatenatorNode")),
		*Reflected(Cue, TEXT("bHasPlayWhenSilent"))));
	Lines.Add(FString::Printf(TEXT("status %s"), *Serialize(SC::DescribeCompileStatus(Cue))));
	return FString::Join(Lines, TEXT("\n"));
}

inline FString FirstLineDifference(const FString& A, const FString& B)
{
	TArray<FString> Left;
	TArray<FString> Right;
	A.ParseIntoArrayLines(Left, false);
	B.ParseIntoArrayLines(Right, false);
	for (int32 Index = 0; Index < FMath::Max(Left.Num(), Right.Num()); ++Index)
	{
		const FString L = Left.IsValidIndex(Index) ? Left[Index] : FString(TEXT("<none>"));
		const FString R = Right.IsValidIndex(Index) ? Right[Index] : FString(TEXT("<none>"));
		if (L != R)
		{
			return FString::Printf(TEXT("line %d:\n  %s\n  %s"), Index, *L, *R);
		}
	}
	return FString();
}

// §6.2 from an extraction: Create on Created, then document / graph / runtime tree / derived / status equal, Diff
// empty, the unchanged re-apply a no-op, and the reloaded asset extracts the same.
struct FRoundTrip
{
	FAssetDocumentResult Applied;
	TSharedPtr<FJsonObject> Recreated;
	double CreateMs = 0.0;
	double NewExtractMs = 0.0;
	double NoOpMs = 0.0;
	double ReloadMs = 0.0;
	TArray<FString> Problems;

	FString Timings() const
	{
		return FString::Printf(TEXT("create %.0f ms, extract new %.0f ms, no-op re-apply %.0f ms, reload extract %.0f ms"), CreateMs, NewExtractMs, NoOpMs, ReloadMs);
	}
};

inline FRoundTrip RoundTrip(FAutomationTestBase& Test, const USoundCue& Original, const TSharedPtr<FJsonObject>& Extracted, const FTarget& Created, const FString& Name)
{
	FRoundTrip Trip;
	double Start = FPlatformTime::Seconds();
	Trip.Applied = Apply(Created, ForTarget(Extracted, Created, TEXT("Create")));
	Trip.CreateMs = Milliseconds(Start);
	if (!Trip.Applied.IsSuccess())
	{
		Trip.Problems.Add(FString::Printf(TEXT("Create failed: %s"), *DiagnosticsText(Trip.Applied)));
		return Trip;
	}
	Start = FPlatformTime::Seconds();
	const FAssetDocumentResult NewResult = ExtractResult(Created.ObjectPath);
	Trip.NewExtractMs = Milliseconds(Start);
	Trip.Recreated = NewResult.IsSuccess() ? NewResult.Payload : nullptr;
	if (!Trip.Recreated.IsValid())
	{
		Trip.Problems.Add(FString::Printf(TEXT("extract of the created cue failed: %s"), *DiagnosticsText(NewResult)));
		return Trip;
	}
	if (WithoutTarget(Extracted) != WithoutTarget(Trip.Recreated))
	{
		Trip.Problems.Add(FString::Printf(TEXT("the created cue extracts differently %s"), *FirstDifference(WithoutTarget(Extracted), WithoutTarget(Trip.Recreated))));
	}
	if (const USoundCue* NewCue = Created.Cue())
	{
		const FString Expected = CueForm(Original);
		const FString Actual = CueForm(*NewCue);
		if (Expected != Actual)
		{
			Trip.Problems.Add(FString::Printf(TEXT("graph / runtime tree / derived / status differ %s"), *FirstLineDifference(Expected, Actual)));
		}
	}
	FString DiffText;
	const TSharedPtr<FJsonObject> Update = ForTarget(Extracted, Created, TEXT("Update"));
	const int32 Changes = DiffCount(Update, DiffText);
	if (Changes != 0)
	{
		Trip.Problems.Add(FString::Printf(TEXT("Diff after Create reports %d changes: %s"), Changes, *DiffText.Left(600)));
	}
	UObject* NewAsset = Created.Get();
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Created.PackageFile);
	Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Again = Apply(Created, Update);
	Trip.NoOpMs = Milliseconds(Start);
	if (!Again.IsSuccess() || Again.bSavedAsset || (NewAsset != nullptr && NewAsset->GetOutermost()->IsDirty())
		|| IFileManager::Get().GetTimeStamp(*Created.PackageFile) != Stamp)
	{
		Trip.Problems.Add(FString::Printf(TEXT("the unchanged re-apply is not a no-op: %s saved=%d"), *DiagnosticsText(Again), Again.bSavedAsset ? 1 : 0));
	}
	if (NewAsset != nullptr)
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({NewAsset->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		if (LoadQuiet(Created.ObjectPath) == nullptr)
		{
			Trip.Problems.Add(TEXT("reload failed"));
		}
		else
		{
			Start = FPlatformTime::Seconds();
			const FString AfterReload = Serialize(Extract(Test, Created.ObjectPath, Name + TEXT(" reloaded")));
			Trip.ReloadMs = Milliseconds(Start);
			if (AfterReload != Serialize(Trip.Recreated))
			{
				Trip.Problems.Add(FString::Printf(TEXT("extraction after reload differs %s"), *FirstDifference(Serialize(Trip.Recreated), AfterReload)));
			}
		}
	}
	return Trip;
}
} // namespace AssetDocumentSoundCueTests
#endif // WITH_DEV_AUTOMATION_TESTS
