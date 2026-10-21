// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Helpers of the MetaSound apply tests (AssetDocumentMetaSoundApplyTests.cpp, AssetDocumentMetaSoundM2Tests.cpp,
// AssetDocumentMetaSoundM3Tests.cpp):
// document editing, the editor side of the free edits (FEditorSide: the editor's builder calls in a transaction,
// then its registration) and the comparison of a document-edited and an editor-edited graph.

#pragma once

#include "AssetDocumentMetaSoundTestSupport.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "MetaSound/AssetDocumentMetaSound.h"
#include "MetasoundAssetSubsystem.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundSettings.h"
#include "MetasoundSource.h"
#include "Misc/App.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "TickableEditorObject.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMetaSoundApplyTests
{
using namespace AssetDocumentMetaSoundTests;

const TCHAR* const PatchClass = TEXT("/Script/MetasoundEngine.MetaSoundPatch");
const TCHAR* const Loop = TEXT("/Metasound/DefaultAssets/Sources/MSS_PlayRandom_Loop.MSS_PlayRandom_Loop");
const TCHAR* const Opera = TEXT("/Game/DemoTemplate/Metasounds/MS_Opera.MS_Opera");

// The id an entry without "Id" gets (metasound.keyed_id): SHA-1 of the seed, four little-endian words.
inline FGuid KeyedId(const FString& Seed)
{
	const FTCHARToUTF8 Utf8(*Seed);
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest);
	uint32 Words[4];
	for (int32 Word = 0; Word < 4; ++Word)
	{
		Words[Word] = static_cast<uint32>(Digest[Word * 4]) | (static_cast<uint32>(Digest[Word * 4 + 1]) << 8)
			| (static_cast<uint32>(Digest[Word * 4 + 2]) << 16) | (static_cast<uint32>(Digest[Word * 4 + 3]) << 24);
	}
	return FGuid(Words[0], Words[1], Words[2], Words[3]);
}

inline TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Parent, const FString& Name)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	if (Parent->TryGetObjectField(Name, Found))
	{
		return *Found;
	}
	const TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
	Parent->SetObjectField(Name, Created);
	return Created;
}

inline TSharedPtr<FJsonObject> BodyOf(const TSharedPtr<FJsonObject>& Document) { return Child(Document, TEXT("Body")); }
inline TSharedPtr<FJsonObject> PageOf(const TSharedPtr<FJsonObject>& Document) { return Child(Child(BodyOf(Document), TEXT("Graphs")), TEXT("Default")); }
inline TSharedPtr<FJsonObject> NodesOf(const TSharedPtr<FJsonObject>& Document) { return Child(PageOf(Document), TEXT("Nodes")); }
inline TSharedPtr<FJsonObject> NodeOf(const TSharedPtr<FJsonObject>& Document, const FString& Key) { return Child(NodesOf(Document), Key); }

inline TSharedPtr<FJsonObject> Json(const TCHAR* Text)
{
	return Parse(Text);
}

inline TArray<TSharedPtr<FJsonValue>> Point(double X, double Y)
{
	return {MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(Y)};
}

// The document as an Update of Target.
inline TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

// The output nodes' ids aside (interface outputs get theirs from the interface).
inline FString WithoutOutputIds(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Copy = Parse(WithoutTarget(Document));
	if (!Copy.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	if (Copy->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetObjectField(TEXT("Graphs"), Graphs))
	{
		for (const auto& Page : (*Graphs)->Values)
		{
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			if (Page.Value->AsObject()->TryGetObjectField(TEXT("Nodes"), Nodes))
			{
				for (const auto& Node : (*Nodes)->Values)
				{
					if (Node.Value->AsObject()->GetStringField(TEXT("Class")) == TEXT("Output"))
					{
						Node.Value->AsObject()->RemoveField(TEXT("Id"));
					}
				}
			}
		}
	}
	return Serialize(Copy);
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

// Applies Document to Target and reports the failure.
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

// A refused apply: its code and path, and an unchanged asset.
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

inline int32 BuilderObjectCount()
{
	int32 Count = 0;
	for (TObjectIterator<UMetaSoundBuilderBase> It; It; ++It)
	{
		Count += IsValid(*It) && !It->HasAnyFlags(RF_ClassDefaultObject) ? 1 : 0;
	}
	return Count;
}

inline bool AssetManagerKnows(const FString& PackageName)
{
	bool bFound = false;
	if (const Metasound::Engine::FMetaSoundAssetManager* Manager = Metasound::Engine::FMetaSoundAssetManager::Get())
	{
		Manager->IterateAssetTagData([&bFound, &PackageName](const Metasound::Frontend::FMetaSoundAssetClassInfo& Info)
		{
			bFound |= Info.AssetPath.GetPackageName().ToString() == PackageName;
		}, true);
	}
	return bFound;
}

// §6.2 from an extraction (OriginalStatus its validation): Create on Created, which then extracts the same (output
// node ids aside) with the same validation; Diff is empty; the unchanged re-apply saves and dirties nothing; the new
// asset extracts the same after a reload. A refused Create returns with Applied failed for the caller to judge.
struct FRoundTrip
{
	FAssetDocumentResult Applied;
	TSharedPtr<FJsonObject> Recreated;
	FString Status;
	double CreateMs = 0.0;
	double NewExtractMs = 0.0;
	double NoOpMs = 0.0;
	double ReloadMs = 0.0;
	TArray<FString> Problems;

	FString Timings() const
	{
		return FString::Printf(TEXT("create %.0f ms, extract new %.0f ms, no-op re-apply %.0f ms, reload extract %.0f ms"), CreateMs, NewExtractMs,
			NoOpMs, ReloadMs);
	}
};

inline FRoundTrip RoundTrip(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Extracted, const FString& OriginalStatus, const FTarget& Created,
	const FString& Name)
{
	FRoundTrip Trip;
	const TSharedPtr<FJsonObject> Document = ForTarget(Extracted, Created, TEXT("Create"));
	double Start = FPlatformTime::Seconds();
	Trip.Applied = Apply(Created, Document);
	Trip.CreateMs = Milliseconds(Start);
	if (!Trip.Applied.IsSuccess())
	{
		return Trip;
	}
	Start = FPlatformTime::Seconds();
	const FAssetDocumentResult NewResult = ExtractResult(Created.ObjectPath);
	Trip.NewExtractMs = Milliseconds(Start);
	Trip.Recreated = NewResult.IsSuccess() ? NewResult.Payload : nullptr;
	if (!Trip.Recreated.IsValid())
	{
		Trip.Problems.Add(FString::Printf(TEXT("extract of the created asset failed: %s"), *DiagnosticsText(NewResult)));
		return Trip;
	}
	const FString Expected = WithoutOutputIds(Extracted);
	const FString Actual = WithoutOutputIds(Trip.Recreated);
	if (Expected != Actual)
	{
		Trip.Problems.Add(FString::Printf(TEXT("the created asset extracts differently %s"), *FirstDifference(Expected, Actual)));
	}
	Trip.Status = StatusOf(NewResult);
	if (OriginalStatus != Trip.Status)
	{
		Trip.Problems.Add(FString::Printf(TEXT("validation differs: '%s' vs '%s'"), *OriginalStatus, *Trip.Status));
	}
	FString DiffText;
	const TSharedPtr<FJsonObject> Update = ForTarget(Extracted, Created, TEXT("Update"));
	const int32 Changes = DiffCount(Update, DiffText);
	if (Changes != 0)
	{
		Trip.Problems.Add(FString::Printf(TEXT("Diff after Create reports %d changes: %s"), Changes, *DiffText.Left(600)));
	}
	// The unchanged re-apply: nothing saved, dirtied or registered.
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
	// Reloaded from disk.
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

// ---- the editor side of the free edits ----------------------------------------------------------------------

// One editor edit: a transaction, Modify, the asset's document builder; on scope exit the registration the
// editor does after its edits (FGraphBuilder::RegisterGraphWithFrontend through the editor subsystem).
struct FEditorSide
{
	UObject& Asset;
	FScopedTransaction Transaction;
	FMetaSoundFrontendDocumentBuilder& Builder;

	explicit FEditorSide(UObject& InAsset)
		: Asset(InAsset)
		, Transaction(NSLOCTEXT("AssetDocumentMetaSoundTests", "EditorEdit", "MetaSound editor edit"))
		, Builder((InAsset.Modify(), Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(InAsset).GetBuilder()))
	{
	}

	~FEditorSide()
	{
		UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(Asset, true);
		if (IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(&Asset))
		{
			Document->ConformObjectToDocument();
		}
		Asset.MarkPackageDirty();
	}

	static FGuid Id(const TSharedPtr<FJsonObject>& Document, const FString& Key)
	{
		FGuid Id;
		FGuid::Parse(NodeOf(Document, Key)->GetStringField(TEXT("Id")), Id);
		return Id;
	}

	FGuid OutputNode(const TCHAR* Member) const
	{
		const FMetasoundFrontendNode* Node = Builder.FindGraphOutputNode(Member);
		return Node != nullptr ? Node->GetID() : FGuid();
	}

	const FMetasoundFrontendVertex* Output(const FGuid& Node, const TCHAR* Pin) const
	{
		if (Pin == nullptr)
		{
			const FMetasoundFrontendNode* Found = Builder.FindNode(Node);
			return Found != nullptr && !Found->Interface.Outputs.IsEmpty() ? &Found->Interface.Outputs[0] : nullptr;
		}
		return Builder.FindNodeOutput(Node, FName(Pin));
	}

	// The schema's connection: the input's edge is replaced.
	void Connect(const FGuid& From, const TCHAR* FromPin, const FGuid& To, const TCHAR* ToPin)
	{
		const FMetasoundFrontendVertex* Out = Output(From, FromPin);
		const FMetasoundFrontendVertex* In = Builder.FindNodeInput(To, FName(ToPin));
		if (!ensure(Out != nullptr && In != nullptr))
		{
			return;
		}
		FMetasoundFrontendEdge Edge;
		Edge.FromNodeID = From;
		Edge.FromVertexID = Out->VertexID;
		Edge.ToNodeID = To;
		Edge.ToVertexID = In->VertexID;
		Builder.RemoveEdgeToNodeInput(To, In->VertexID);
		Builder.AddEdge(MoveTemp(Edge));
	}

	void Disconnect(const FGuid& To, const TCHAR* ToPin)
	{
		if (const FMetasoundFrontendVertex* In = Builder.FindNodeInput(To, FName(ToPin)))
		{
			Builder.RemoveEdgeToNodeInput(To, In->VertexID);
		}
	}

	FGuid AddNode(const TCHAR* ClassName, double X, double Y)
	{
		FMetasoundFrontendClassName Name;
		FMetasoundFrontendClassName::Parse(ClassName, Name);
		const FMetasoundFrontendNode* Node = Builder.AddNodeByClassName(Name, 1);
		if (!ensure(Node != nullptr))
		{
			return FGuid();
		}
		const FGuid Id = Node->GetID();
		Builder.SetNodeLocation(Id, FVector2D(X, Y));
		return Id;
	}

	void Move(const FGuid& Node, double X, double Y)
	{
		Builder.SetNodeLocation(Node, FVector2D(X, Y));
	}

	// The Interfaces panel (MetasoundDetailCustomization): ModifyInterfaces without default node locations.
	void Interface(const TCHAR* Name, bool bAdd)
	{
		using namespace Metasound::Frontend;
		TArray<FMetasoundFrontendVersion> Remove;
		TArray<FMetasoundFrontendVersion> Add;
		if (bAdd)
		{
			FMetasoundFrontendInterface Found;
			if (ensure(ISearchEngine::Get().FindInterfaceWithHighestVersion(Name, Found)))
			{
				Add.Add(Found.Metadata.Version);
			}
		}
		else
		{
			for (const FMetasoundFrontendVersion& Version : Builder.GetConstDocumentChecked().Interfaces)
			{
				if (Version.Name == FName(Name))
				{
					Remove.Add(Version);
				}
			}
		}
		FModifyInterfaceOptions Options(Remove, Add);
		Options.bSetDefaultNodeLocations = false;
		Builder.ModifyInterfaces(MoveTemp(Options));
	}
};

// The Details panel's edit of a Source's OutputFormat (PostEditChangeOutputFormat).
inline void SetOutputFormat(UObject& Asset, EMetaSoundOutputAudioFormat Format)
{
	UMetaSoundSource* Source = CastChecked<UMetaSoundSource>(&Asset);
	FProperty* Property = UMetaSoundSource::StaticClass()->FindPropertyByName(TEXT("OutputFormat"));
	const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentMetaSoundTests", "OutputFormat", "Output Format"));
	Source->Modify();
	Source->PreEditChange(Property);
	Source->OutputFormat = Format;
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Source->PostEditChangeProperty(Event);
}

// The pages of a document (presets have none).
inline TArray<FString> PagesOf(const TSharedPtr<FJsonObject>& Document)
{
	TArray<FString> Pages;
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	if (Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetObjectField(TEXT("Graphs"), Graphs))
	{
		AssetDocumentServerJson::GetKeys(**Graphs, Pages);
	}
	return Pages;
}

inline TSharedPtr<FJsonObject> PageNodes(const TSharedPtr<FJsonObject>& Document, const FString& Page)
{
	return Child(Child(Child(BodyOf(Document), TEXT("Graphs")), Page), TEXT("Nodes"));
}

// The graph of a document independent of its key numbering: on every page nodes by id (output nodes by member),
// sources as "<id>.<pin>". Ids of the editor-made nodes are first mapped to the document-made ones (same page, Class,
// Member and Position), cumulatively in IdMap.
inline FString GraphForm(const TSharedPtr<FJsonObject>& Document, const TMap<FString, FString>& IdMap)
{
	const TSharedPtr<FJsonObject> Copy = Parse(WithoutTarget(Document));
	for (const FString& Page : PagesOf(Copy))
	{
		const TSharedPtr<FJsonObject> Nodes = PageNodes(Copy, Page);
		TMap<FString, FString> KeyToId;
		for (const auto& Node : Nodes->Values)
		{
			const TSharedPtr<FJsonObject> Entry = Node.Value->AsObject();
			const FString Id = Entry->GetStringField(TEXT("Id"));
			KeyToId.Add(FString(*Node.Key), Entry->GetStringField(TEXT("Class")) == TEXT("Output") ? TEXT("output:") + Entry->GetStringField(TEXT("Member"))
				: IdMap.Contains(Id) ? IdMap[Id] : Id);
		}
		const TSharedPtr<FJsonObject> ById = MakeShared<FJsonObject>();
		TArray<FString> Keys;
		AssetDocumentServerJson::GetKeys(*Nodes, Keys);
		for (const FString& Key : Keys)
		{
			const TSharedPtr<FJsonObject> Entry = Nodes->GetObjectField(Key);
			Entry->RemoveField(TEXT("Id"));
			const TSharedPtr<FJsonObject>* Inputs = nullptr;
			if (Entry->TryGetObjectField(TEXT("Inputs"), Inputs))
			{
				for (auto& Input : (*Inputs)->Values)
				{
					FString Source = Input.Value->AsString();
					FString SourceKey = Source;
					FString Pin;
					Source.Split(TEXT("."), &SourceKey, &Pin);
					const FString* SourceId = KeyToId.Find(SourceKey.IsEmpty() ? Source : SourceKey);
					Input.Value = MakeShared<FJsonValueString>((SourceId != nullptr ? *SourceId : Source) + (Pin.IsEmpty() ? FString() : TEXT(".") + Pin));
				}
			}
			ById->SetObjectField(KeyToId[Key], Entry);
		}
		AssetDocumentServerJson::SortByKey(*ById);
		Child(Child(BodyOf(Copy), TEXT("Graphs")), Page)->SetObjectField(TEXT("Nodes"), ById);
	}
	return Serialize(Copy);
}

inline void MapIds(const TSharedPtr<FJsonObject>& DocumentSide, const TSharedPtr<FJsonObject>& EditorSide, TMap<FString, FString>& IdMap)
{
	auto Signature = [](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Member;
		Entry->TryGetStringField(TEXT("Member"), Member);
		const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
		FString At = TEXT("hidden");
		if (Entry->TryGetArrayField(TEXT("Position"), Position) && Position->Num() == 2)
		{
			At = FString::Printf(TEXT("%.1f,%.1f"), (*Position)[0]->AsNumber(), (*Position)[1]->AsNumber());
		}
		return Entry->GetStringField(TEXT("Class")) + TEXT("|") + Member + TEXT("|") + At;
	};
	const TArray<FString> EditorPages = PagesOf(EditorSide);
	for (const FString& Page : PagesOf(DocumentSide))
	{
		if (!EditorPages.Contains(Page))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> DocumentNodes = PageNodes(DocumentSide, Page);
		const TSharedPtr<FJsonObject> EditorNodes = PageNodes(EditorSide, Page);
		TMap<FString, FString> Free;
		TSet<FString> DocumentIds;
		for (const auto& Node : DocumentNodes->Values)
		{
			DocumentIds.Add(Node.Value->AsObject()->GetStringField(TEXT("Id")));
		}
		TSet<FString> Mapped;
		for (const TPair<FString, FString>& Pair : IdMap)
		{
			Mapped.Add(Pair.Value);
		}
		TSet<FString> EditorIds;
		for (const auto& Node : EditorNodes->Values)
		{
			EditorIds.Add(Node.Value->AsObject()->GetStringField(TEXT("Id")));
		}
		for (const auto& Node : DocumentNodes->Values)
		{
			const FString Id = Node.Value->AsObject()->GetStringField(TEXT("Id"));
			if (!EditorIds.Contains(Id) && !Mapped.Contains(Id))
			{
				Free.Add(Signature(Node.Value->AsObject()), Id);
			}
		}
		for (const auto& Node : EditorNodes->Values)
		{
			const FString Id = Node.Value->AsObject()->GetStringField(TEXT("Id"));
			if (DocumentIds.Contains(Id) || IdMap.Contains(Id))
			{
				continue;
			}
			if (const FString* Match = Free.Find(Signature(Node.Value->AsObject())))
			{
				IdMap.Add(Id, *Match);
				Free.Remove(Signature(Node.Value->AsObject()));
			}
		}
	}
}

// The key extraction gave the node of Class at (X, Y) (a node added by an earlier edit is keyed by its class then).
inline FString KeyAt(const TSharedPtr<FJsonObject>& Document, const TCHAR* Class, double X, double Y)
{
	for (const auto& Node : NodesOf(Document)->Values)
	{
		const TSharedPtr<FJsonObject> Entry = Node.Value->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
		if (Entry->GetStringField(TEXT("Class")) == Class && Entry->TryGetArrayField(TEXT("Position"), Position) && Position->Num() == 2
			&& FMath::IsNearlyEqual((*Position)[0]->AsNumber(), X) && FMath::IsNearlyEqual((*Position)[1]->AsNumber(), Y))
		{
			return FString(*Node.Key);
		}
	}
	return FString();
}

// One §6.3 edit: the document change (on the document side's extraction) and the editor's calls.
struct FFreeEdit
{
	FString Name;
	TFunction<void(const TSharedPtr<FJsonObject>&)> Document;
	TFunction<void(UObject&, const TSharedPtr<FJsonObject>&)> Editor;
};

inline TSharedPtr<FJsonObject> SetInputs(const TSharedPtr<FJsonObject>& Document, const FString& Key, const FString& Pin, const FString& Source)
{
	const TSharedPtr<FJsonObject> Inputs = Child(NodeOf(Document, Key), TEXT("Inputs"));
	if (Source.IsEmpty())
	{
		Inputs->RemoveField(Pin);
	}
	else
	{
		Inputs->SetStringField(Pin, Source);
	}
	return Inputs;
}
// ---- shared by the M2 / M3 tests ------------------------------------------------------------------------------

inline bool Save(FAutomationTestBase& Test, UObject* Asset, const FString& Stage)
{
	const bool bSaved = Asset != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Asset->GetOutermost()}, false);
	Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Stage), bSaved);
	ReleaseUndoHistory();
	return bSaved;
}


inline bool LeftNothing(const FTarget& Target)
{
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TArray<FAssetData> Assets;
	FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(FName(*Target.PackageName), Assets);
	return Target.Get() == nullptr && !IFileManager::Get().FileExists(*Target.PackageFile) && Assets.IsEmpty() && !AssetManagerKnows(Target.PackageName);
}


// Every undo step keeps the asset extractable; every undone step redoes.
inline void UndoRedoAll(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	int32 Undone = 0;
	while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
	{
		++Undone;
		Test.TestTrue(*FString::Printf(TEXT("%s: whole after undo %d"), *Stage, Undone), ExtractResult(Target.ObjectPath).IsSuccess());
	}
	int32 Redone = 0;
	while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
	{
		++Redone;
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), *Stage), Redone, Undone);
}

// Both sides start from the same applied state (the open-time injection made, both registered).
inline bool StartBoth(FAutomationTestBase& Test, const FTarget& DocumentSide, const FTarget& EditorSide, const FString& Stage)
{
	const TSharedPtr<FJsonObject> Start = Extract(Test, DocumentSide.ObjectPath, Stage + TEXT(" start"));
	if (!Start.IsValid())
	{
		return false;
	}
	Child(BodyOf(Start), TEXT("Metadata"))->SetStringField(TEXT("Description"), TEXT("M2 free edits"));
	return ApplyOk(Test, DocumentSide, ForTarget(Start, DocumentSide, TEXT("Update")), Stage + TEXT(" start (document side)"))
		&& ApplyOk(Test, EditorSide, ForTarget(Start, EditorSide, TEXT("Update")), Stage + TEXT(" start (editor side)"));
}

// Applies the document edit to DocumentSide and the editor's calls to EditorSide; both then extract the same graph
// (editor-made node ids mapped, cumulatively in IdMap) and validate the same.
inline bool CompareFreeEdit(FAutomationTestBase& Test, const FTarget& DocumentSide, const FTarget& EditorSide, const FFreeEdit& Edit, TMap<FString, FString>& IdMap,
	const FString& Name)
{
	const FString Stage = Name + TEXT(": ") + Edit.Name;
	const TSharedPtr<FJsonObject> Before = Extract(Test, DocumentSide.ObjectPath, Stage + TEXT(" (document side, before)"));
	const TSharedPtr<FJsonObject> EditorBefore = Extract(Test, EditorSide.ObjectPath, Stage + TEXT(" (editor side, before)"));
	if (!Before.IsValid() || !EditorBefore.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Edited = ForTarget(Before, DocumentSide, TEXT("Update"));
	Edit.Document(Edited);
	const double Start = FPlatformTime::Seconds();
	if (!ApplyOk(Test, DocumentSide, Edited, Stage))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: document %s"), *Stage, *Serialize(Edited)));
		return false;
	}
	const double ApplyMs = Milliseconds(Start);
	Edit.Editor(*EditorSide.Get(), EditorBefore);
	const FAssetDocumentResult DocumentResult = ExtractResult(DocumentSide.ObjectPath);
	const FAssetDocumentResult EditorResult = ExtractResult(EditorSide.ObjectPath);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: both sides extract"), *Stage), DocumentResult.IsSuccess() && EditorResult.IsSuccess()))
	{
		return false;
	}
	MapIds(DocumentResult.Payload, EditorResult.Payload, IdMap);
	const FString Expected = GraphForm(EditorResult.Payload, IdMap);
	const FString Actual = GraphForm(DocumentResult.Payload, {});
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: document and editor give the same asset"), *Stage), Actual, Expected))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *FirstDifference(Actual, Expected)));
		Test.AddInfo(FString::Printf(TEXT("%s: document side %s"), *Stage, *Serialize(DocumentResult.Payload)));
		Test.AddInfo(FString::Printf(TEXT("%s: editor side %s"), *Stage, *Serialize(EditorResult.Payload)));
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: same validation"), *Stage), StatusOf(DocumentResult), StatusOf(EditorResult));
	Test.AddInfo(FString::Printf(TEXT("%s: applied in %.0f ms"), *Stage, ApplyMs));
	return true;
}


inline void IgnoreUnregisteredMidiNoteTrigger(FAutomationTestBase& Test)
{
	// Only the engine's log lines (a test's own errors never contain them).
	Test.AddExpectedMessagePlain(TEXT("Could not find node [RegistryKey:External_HarmonixNodes.MidiNoteTrigger_0.1]"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, -1);
	Test.AddExpectedMessagePlain(TEXT("NodeName:MidiNoteTrigger] from registry [Class: HarmonixNodes.MidiNoteTrigger v0.1]"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, -1);
	Test.AddExpectedMessagePlain(TEXT("Failed to build MetaSound graph in asset '"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1);
	// Some engines also try to auto-update the node on load and log that the version is missing.
	Test.AddExpectedMessagePlain(TEXT("Final node class 'External_HarmonixNodes.MidiNoteTrigger_0.1' does not exist in the registry"),
		ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1);
}

inline void TickEditor()
{
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		FTickableEditorObject::TickObjects(0.f);
	}
}

inline bool MetaSoundIsOpen(UAssetEditorSubsystem& Editors, UObject& Asset)
{
	return Editors.FindEditorForAsset(&Asset, false) != nullptr;
}

inline UEdGraph* EditorGraphOf(UObject& Asset)
{
	const Metasound::Frontend::IMetaSoundAssetManager* Manager = Metasound::Frontend::IMetaSoundAssetManager::Get();
	FMetasoundAssetBase* MetaSound = Manager != nullptr ? Manager->GetAsAsset(Asset) : nullptr;
	return MetaSound != nullptr ? MetaSound->GetGraph() : nullptr;
}

// Without an audio device, opening a MetaSound editor while a MetaSound with Harmonix Fusion patch literals is loaded
// in the session crashes the engine (Harmonix KeyzoneSettings.cpp:48 check(SoundWaveProxy), reached from the editor's
// open: GraphEditor -> MetasoundEditor -> MetasoundEngine -> MetasoundFrontend -> HarmonixMetasound). Such MetaSounds
// (loaded by earlier tests) are unloaded first; Keep stays.
inline void UnloadMetaSoundsNeedingAudio(FAutomationTestBase& Test, const UObject& Keep)
{
	if (FApp::CanEverRenderAudio())
	{
		return;
	}
	TArray<UPackage*> Unload;
	TArray<FString> Names;
	for (TObjectIterator<UObject> It; It; ++It)
	{
		UObject* Object = *It;
		if (IsValid(Object) && Object->IsAsset() && Cast<IMetaSoundDocumentInterface>(Object) != nullptr && Object->GetOutermost() != Keep.GetOutermost()
			&& !UE::AssetDocument::Graph::Audio::MetaSound::FindLiteralNeedingAudioDevice(*Object).IsEmpty())
		{
			Unload.AddUnique(Object->GetOutermost());
			Names.Add(Object->GetPathName());
		}
	}
	if (!Unload.IsEmpty())
	{
		Test.AddInfo(FString::Printf(TEXT("unloaded before opening the editor (no audio device): %s"), *FString::Join(Names, TEXT(", "))));
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages(Unload);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
}

// ---- project page settings (M3) -----------------------------------------------------------------------------

// MetaSound pages of the project for one test, set as Project Settings > Audio > MetaSound > Pages sets them
// (PreEditChange, the PageSettings array changed, PostEditChangeProperty: ConformPageSettings, OnPageSettingsUpdated,
// the cached page order). In memory only: DefaultMetaSound.ini is not written, and the previous pages come back on
// scope exit. A project enables the same pages with [/Script/MetasoundEngine.MetaSoundSettings]
// +PageSettings=(UniqueId=<guid>,Name="<name>") in Config/DefaultMetaSound.ini.
class FScopedProjectPages
{
public:
	explicit FScopedProjectPages(const TArray<TPair<FName, FGuid>>& Pages)
	{
		Settings = GetMutableDefault<UMetaSoundSettings>();
		Property = FindFProperty<FArrayProperty>(UMetaSoundSettings::StaticClass(), UMetaSoundSettings::GetPageSettingPropertyName());
		if (Settings == nullptr || Property == nullptr)
		{
			return;
		}
		Saved = Settings->GetProjectPageSettings();
		Settings->PreEditChange(Property);
		for (const TPair<FName, FGuid>& Page : Pages)
		{
			if (Settings->FindPageSettings(Page.Key) == nullptr && Settings->FindPageSettings(Page.Value) == nullptr)
			{
				FMetaSoundPageSettings& Added = Array().AddDefaulted_GetRef();
				Added.UniqueId = Page.Value;
				Added.Name = Page.Key;
			}
		}
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ArrayAdd);
		// UMetaSoundSettings overrides it privately; the settings editor calls it through UObject.
		static_cast<UObject*>(Settings)->PostEditChangeProperty(Event);
	}

	~FScopedProjectPages()
	{
		if (Settings == nullptr || Property == nullptr)
		{
			return;
		}
		Settings->PreEditChange(Property);
		Array() = Saved;
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		// UMetaSoundSettings overrides it privately; the settings editor calls it through UObject.
		static_cast<UObject*>(Settings)->PostEditChangeProperty(Event);
	}

	FScopedProjectPages(const FScopedProjectPages&) = delete;
	FScopedProjectPages& operator=(const FScopedProjectPages&) = delete;

	bool IsValid() const { return Settings != nullptr && Property != nullptr; }

private:
	TArray<FMetaSoundPageSettings>& Array() const { return *Property->ContainerPtrToValuePtr<TArray<FMetaSoundPageSettings>>(Settings); }

	UMetaSoundSettings* Settings = nullptr;
	FArrayProperty* Property = nullptr;
	TArray<FMetaSoundPageSettings> Saved;
};

// The Details panel's Add Page Graph (MetasoundDetailCustomization: a transaction, Modify, the builder's Modify,
// AddGraphPage duplicating the last page and making it the build page), then the editor's registration.
inline void EditorAddPage(UObject& Asset, FName Page)
{
	FEditorSide Side(Asset);
	UMetaSoundBuilderBase& Builder = Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(Asset);
	Builder.Modify();
	EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
	Builder.AddGraphPage(Page, true, true, Result);
	ensure(Result == EMetaSoundBuilderResult::Succeeded);
}

// The Details panel's page remove button (Modify, the builder's Modify, RemoveGraphPage).
inline void EditorRemovePage(UObject& Asset, const FGuid& PageID)
{
	FEditorSide Side(Asset);
	Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(Asset).Modify();
	ensure(Side.Builder.RemoveGraphPage(PageID));
}

// The key of the node of Class at (X, Y) on Page.
inline FString KeyAtOnPage(const TSharedPtr<FJsonObject>& Document, const FString& Page, const TCHAR* Class, double X, double Y)
{
	for (const auto& Node : PageNodes(Document, Page)->Values)
	{
		const TSharedPtr<FJsonObject> Entry = Node.Value->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
		if (Entry->GetStringField(TEXT("Class")) == Class && Entry->TryGetArrayField(TEXT("Position"), Position) && Position->Num() == 2
			&& FMath::IsNearlyEqual((*Position)[0]->AsNumber(), X) && FMath::IsNearlyEqual((*Position)[1]->AsNumber(), Y))
		{
			return FString(*Node.Key);
		}
	}
	return FString();
}

inline FGuid NodeIdOnPage(const TSharedPtr<FJsonObject>& Document, const FString& Page, const FString& Key)
{
	FGuid Id;
	FGuid::Parse(Child(PageNodes(Document, Page), Key)->GetStringField(TEXT("Id")), Id);
	return Id;
}
} // namespace AssetDocumentMetaSoundApplyTests
#endif // WITH_DEV_AUTOMATION_TESTS
