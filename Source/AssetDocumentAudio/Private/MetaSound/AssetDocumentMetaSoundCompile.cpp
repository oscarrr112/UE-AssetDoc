// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// audio.metasound.compile.v1 (design §5.1): what the MetaSound editor does after its edits.
//   1. Register: UMetaSoundEditorSubsystem::RegisterGraphWithFrontend(Asset, bForceViewSync=true)
//      (FGraphBuilder::RegisterGraphWithFrontend -> FMetasoundAssetBase::UpdateAndRegisterForExecution: referenced
//      asset classes, dependencies, auto-update of node minor versions, registry metadata, graph class), then
//      ConformObjectToDocument (UObject data that mirrors the document, e.g. a Source's OutputFormat) and a wait for
//      async graph registration. No audio device is involved (-NoSound editors register the same way).
//   2. Validate without an editor graph (every page), then build the registered graph into an operator as a generator
//      does when the sound plays: DescribeCompileStatus (AssetDocumentMetaSoundCompileStatus.cpp,
//      AssetDocumentMetaSoundBuildCheck.cpp).
//   3. Dependents (design §5.3): loaded MetaSounds that use this one as a node, with their package dirty state
//      before and after registration (registration can re-register and dirty open referencers); for Validate's
//      staged trial copy, those of the asset it copies.
//   {} -> {status: UpToDate|UpToDateWithWarnings|Error, messages[{severity, message, node_id, node_ids[], page,
//          source: Validation|Build}], registered,
//          dependents[{path, dirty_before, dirty_after}]}
//
// The derived-state rebuilder (AssetDocumentAudio.MetaSound) runs after Core restored a package snapshot (rollback):
// Core has already called the asset's PostEditUndo (FAssetHelper::PostEditUndo: ReloadBuilder); the rebuilder adds
// what the editor's undo does with a graph open: register with the frontend and conform the object to the document.

#include "AssetDocumentMetaSound.h"

#include "AssetDocumentAudioPrimitives.h"
#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentEditorTrialScope.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Metasound.h"
#include "MetasoundFrontendDocument.h"
#include "Misc/App.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "MetasoundSettings.h"
#include "MetasoundSource.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocument::Graph::Audio
{
namespace MetaSound
{
namespace
{
struct FDependent
{
	TWeakObjectPtr<UObject> Object;
	FString Path;
	bool bDirtyBefore = false;
};

// Loaded MetaSound assets whose documents use Asset's graph class as a node.
TArray<FDependent> FindLoadedDependents(const UObject& Asset)
{
	TArray<FDependent> Out;
	const IMetaSoundDocumentInterface* Document = AsMetaSound(Asset);
	if (Document == nullptr)
	{
		return Out;
	}
	const FMetasoundFrontendClassName ClassName = Document->GetConstDocument().RootGraph.Metadata.GetClassName();
	auto Visit = [&Out, &Asset, &ClassName](UObject& Candidate)
	{
		if (&Candidate == &Asset || !Candidate.IsAsset() || Candidate.HasAnyFlags(RF_ClassDefaultObject))
		{
			return;
		}
		const IMetaSoundDocumentInterface* Other = AsMetaSound(Candidate);
		if (Other == nullptr)
		{
			return;
		}
		for (const FMetasoundFrontendClass& Dependency : Other->GetConstDocument().Dependencies)
		{
			if (Dependency.Metadata.GetClassName() == ClassName)
			{
				Out.Add({&Candidate, Candidate.GetPathName(), Candidate.GetOutermost()->IsDirty()});
				return;
			}
		}
	};
	for (TObjectIterator<UMetaSoundSource> It; It; ++It)
	{
		Visit(**It);
	}
	for (TObjectIterator<UMetaSoundPatch> It; It; ++It)
	{
		Visit(**It);
	}
	Out.Sort([](const FDependent& A, const FDependent& B) { return A.Path < B.Path; });
	return Out;
}
} // namespace

void RegisterAsTheEditorDoes(UObject& Asset)
{
	UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(Asset, true);
	if (IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(&Asset))
	{
		Document->ConformObjectToDocument();
	}
	using namespace Metasound::Frontend;
	const IMetaSoundAssetManager* Manager = IMetaSoundAssetManager::Get();
	const FMetasoundAssetBase* MetaSound = Manager != nullptr ? Manager->GetAsAsset(static_cast<const UObject&>(Asset)) : nullptr;
	if (MetaSound != nullptr)
	{
		if (INodeClassRegistry* Registry = INodeClassRegistry::Get())
		{
			Registry->WaitForAsyncGraphRegistration(MetaSound->GetGraphRegistryKey());
		}
	}
}
namespace
{
FString FindLiteralNeedingAudioDeviceIn(const UObject& Asset, TSet<const UObject*>& Visited)
{
	FString Found;
	const IMetaSoundDocumentInterface* Document = AsMetaSound(Asset);
	if (Document == nullptr || Visited.Contains(&Asset))
	{
		return Found;
	}
	Visited.Add(&Asset);
	auto Check = [&Found](const UObject* Object)
	{
		for (const UClass* Class = Object != nullptr ? Object->GetClass() : nullptr; Class != nullptr && Found.IsEmpty(); Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == TEXT("FusionPatch"))
			{
				Found = Object->GetPathName();
			}
		}
	};
	const FMetasoundFrontendDocument& Doc = Document->GetConstDocument();
	Metasound::Frontend::ForEachLiteral(Doc, [&Check](const FName&, const FMetasoundFrontendLiteral& Literal)
	{
		UObject* Object = nullptr;
		TArray<UObject*> Objects;
		if (Literal.TryGet(Object))
		{
			Check(Object);
		}
		else if (Literal.TryGet(Objects))
		{
			for (const UObject* Item : Objects)
			{
				Check(Item);
			}
		}
	}, UMetaSoundSettings::GetPageOrder());
	if (!Found.IsEmpty())
	{
		return Found;
	}
	// The MetaSounds this one uses as nodes are registered with it.
	const FAssetClassPaths Paths(Asset);
	for (const FMetasoundFrontendClass& Dependency : Doc.Dependencies)
	{
		const EMetasoundFrontendClassType Type = Dependency.Metadata.GetType();
		if (Type != EMetasoundFrontendClassType::External && Type != EMetasoundFrontendClassType::Graph)
		{
			continue;
		}
		const FString Path = Paths.Find(Dependency.Metadata);
		const UObject* Referenced = Path.IsEmpty() ? nullptr : FSoftObjectPath(Path).TryLoad();
		if (Referenced != nullptr)
		{
			Found = FindLiteralNeedingAudioDeviceIn(*Referenced, Visited);
			if (!Found.IsEmpty())
			{
				return Found;
			}
		}
	}
	return Found;
}
} // namespace

FString FindLiteralNeedingAudioDevice(const UObject& Asset)
{
	if (FApp::CanEverRenderAudio())
	{
		return FString();
	}
	TSet<const UObject*> Visited;
	return FindLiteralNeedingAudioDeviceIn(Asset, Visited);
}
} // namespace MetaSound

bool HandleMetaSoundCompile(FCall& Call, FObjectBuilder& Result)
{
	using namespace MetaSound;
	if (!Call.Args.OnlyFields({}))
	{
		return false;
	}
	if (AsMetaSound(Call.Asset) == nullptr)
	{
		return Fail(Call, TEXT("MetaSound.UnsupportedAsset"), TEXT("MetaSound primitives compile MetaSound Sources and Patches."), TEXT("/HandleArguments/asset"));
	}
	const FString NeedsAudio = FindLiteralNeedingAudioDevice(Call.Asset);
	if (!NeedsAudio.IsEmpty())
	{
		return Fail(Call, TEXT("MetaSound.AudioDeviceRequired"), FString::Printf(TEXT("The MetaSound uses %s; registering it without an audio "
			"device (an editor started with -NoSound) would crash the editor (Harmonix FusionPatch proxies need audio). Run the editor with "
			"audio to edit it."), *NeedsAudio));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	// Validate's trial registers a staged copy under a class name of its own (AssetDocumentMetaSoundTrial.cpp): the
	// dependents are those the apply would register again, the users of the asset it copies.
	const UObject* TrialSource = FAssetDocumentEditorTrialScope::FindTrialSource(Call.Asset);
	const TArray<FDependent> Dependents = FindLoadedDependents(TrialSource != nullptr ? *TrialSource : static_cast<const UObject&>(Call.Asset));
	RegisterAsTheEditorDoes(Call.Asset);
	if (NonInteractive.WasInteractionRefused())
	{
		NonInteractive.DiscardCreatedPackages();
		return Call.Args.Fail(TEXT("MetaSound.InteractiveEditRefused"), TEXT("Registering the MetaSound tried to open an editor dialog, which an MCP "
			"request cannot answer."));
	}

	const TSharedPtr<FJsonObject> Status = DescribeCompileStatus(Call.Asset);
	if (!Status.IsValid())
	{
		return Fail(Call, TEXT("MetaSound.ReadFailed"), TEXT("Could not read the MetaSound's validation state."));
	}
	Result.AddString(TEXT("status"), Status->GetStringField(TEXT("Status")));
	TArray<FValue> Messages;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Status->TryGetArrayField(TEXT("Messages"), Rows))
	{
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			const TSharedPtr<FJsonObject> Object = Row->AsObject();
			FObjectBuilder Message;
			Message.AddString(TEXT("severity"), Object->GetStringField(TEXT("Severity")));
			Message.AddString(TEXT("message"), Object->GetStringField(TEXT("Message")));
			Message.AddString(TEXT("node_id"), Object->GetStringField(TEXT("NodeGuid")));
			Message.AddString(TEXT("page"), Object->GetStringField(TEXT("Graph")));
			Message.AddString(TEXT("source"), Object->GetStringField(TEXT("Source")));
			Message.Add(TEXT("node_ids"), MakeStringArray(GetMessageNodeIds(*Object)));
			Messages.Add(Message.Build());
		}
	}
	Result.Add(TEXT("messages"), MakeArray(MoveTemp(Messages)));
	Result.AddBool(TEXT("registered"), Status->GetBoolField(TEXT("Registered")));
	TArray<FValue> DependentRows;
	for (const FDependent& Dependent : Dependents)
	{
		const UObject* Object = Dependent.Object.Get();
		FObjectBuilder Row;
		Row.AddString(TEXT("path"), Dependent.Path);
		Row.AddBool(TEXT("dirty_before"), Dependent.bDirtyBefore);
		Row.AddBool(TEXT("dirty_after"), Object != nullptr && Object->GetOutermost()->IsDirty());
		DependentRows.Add(Row.Build());
	}
	Result.Add(TEXT("dependents"), MakeArray(MoveTemp(DependentRows)));
	return true;
}

namespace MetaSound
{
namespace
{
class FMetaSoundRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentAudio.MetaSound"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override
	{
		return AsMetaSound(PrimaryAsset) != nullptr;
	}
	// The MetaSound editor's graph (UMetaSoundSource / UMetaSoundPatch::EditorGraph, transient) is a view the open
	// editor rebuilds from the document (FGraphBuilder::SynchronizeGraph on its tick, forced by the registration
	// below): it is neither captured nor restored, and the nodes it gained during the apply are not removed under
	// the open editor.
	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		for (const UObject* Outer = &Object; Outer != nullptr; Outer = Outer->GetOuter())
		{
			if (Outer->IsA<UEdGraph>())
			{
				return EAssetDocumentSnapshotObjectRole::Derived;
			}
		}
		return FAssetDocumentPackageSnapshot::ClassifyDefault(Object);
	}
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		FAssetDocumentContractValidationResult Result;
		// Without an audio device such a MetaSound is left unregistered (as the failed compile left it).
		if (FindLiteralNeedingAudioDevice(PrimaryAsset).IsEmpty())
		{
			RegisterAsTheEditorDoes(PrimaryAsset);
		}
		Result.bValid = true;
		return Result;
	}
};
} // namespace

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeMetaSoundRebuilder()
{
	return MakeShared<FMetaSoundRebuilder>();
}
} // namespace MetaSound
} // namespace UE::AssetDocument::Graph::Audio
