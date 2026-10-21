// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// The editor-open normalisation of the MetaSound read (design §3.3): what MetasoundEditor.cpp does when an
// asset is opened (FindOrBeginBuilding, VersionAsset, FInputNodeTemplate::Inject(bForceNodeCreation=false)),
// done on a transient copy of the document so that reading never changes, dirties or registers the asset.

#include "AssetDocumentMetaSound.h"

#include "Metasound.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendDocumentIdGenerator.h"
#include "MetasoundSource.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateInput.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Audio::MetaSound
{
namespace
{
// UMetaSoundBuilderDocument keeps its copy in a private UPROPERTY; the read renames the template nodes the
// injection created there (the builder is rebuilt afterwards, so its caches never see a stale document).
FMetasoundFrontendDocument* GetMutableCopyDocument(UMetaSoundBuilderDocument& Copy)
{
	const FStructProperty* Property = FindFProperty<FStructProperty>(UMetaSoundBuilderDocument::StaticClass(), TEXT("Document"));
	if (Property == nullptr || Property->Struct != FMetasoundFrontendDocument::StaticStruct())
	{
		return nullptr;
	}
	return Property->ContainerPtrToValuePtr<FMetasoundFrontendDocument>(&Copy);
}

TUniquePtr<FMetaSoundFrontendDocumentBuilder> MakeBuilder(UMetaSoundBuilderDocument& Copy)
{
	const TScriptInterface<IMetaSoundDocumentInterface> Interface(&Copy);
	return MakeUnique<FMetaSoundFrontendDocumentBuilder>(Interface);
}
} // namespace

const IMetaSoundDocumentInterface* AsMetaSound(const UObject& Object)
{
	if (!Object.IsA<UMetaSoundSource>() && !Object.IsA<UMetaSoundPatch>())
	{
		return nullptr;
	}
	return Cast<const IMetaSoundDocumentInterface>(&Object);
}

FGuid MakeInputTemplateNodeId(const FGuid& InputNodeID)
{
	return FGuid::NewDeterministicGuid(FString(TEXT("AssetDocument.MetaSound.InputTemplate:")) + InputNodeID.ToString(EGuidFormats::Digits));
}

FNormalizedDocument::FNormalizedDocument(const UObject& InAsset)
	: Asset(InAsset)
{
	using namespace Metasound::Frontend;

	const IMetaSoundDocumentInterface* Source = AsMetaSound(InAsset);
	if (Source == nullptr)
	{
		return;
	}
	Copy.Reset(&UMetaSoundBuilderDocument::Create(*Source));
	FMetasoundFrontendDocument* MutableDocument = GetMutableCopyDocument(*Copy);
	if (MutableDocument == nullptr)
	{
		Copy.Reset();
		return;
	}

	TMap<FGuid, FGuid> Renames;
	FGuid BuildPageID;
	{
		const FDocumentIDGenerator::FScopeDeterminism Determinism(true);
		Builder = MakeBuilder(*Copy);
		BuildPageID = Builder->GetBuildPageID();
		TSet<FGuid> Before;
		const FMetasoundFrontendGraph* BuildGraph = MutableDocument->RootGraph.FindConstGraph(BuildPageID);
		if (BuildGraph == nullptr)
		{
			return;
		}
		for (const FMetasoundFrontendNode& Node : BuildGraph->Nodes)
		{
			Before.Add(Node.GetID());
		}
		constexpr bool bForceNodeCreation = false;
		if (FInputNodeTemplate::GetChecked().Inject(*Builder, bForceNodeCreation))
		{
			const FMetasoundFrontendGraph& Injected = Builder->FindConstBuildGraphChecked();
			for (const FMetasoundFrontendNode& Node : Injected.Nodes)
			{
				if (Before.Contains(Node.GetID()))
				{
					continue;
				}
				// The template's one input is fed by the hidden input node of the member it shows.
				for (const FMetasoundFrontendEdge& Edge : Injected.Edges)
				{
					if (Edge.ToNodeID == Node.GetID())
					{
						Renames.Add(Node.GetID(), MakeInputTemplateNodeId(Edge.FromNodeID));
						break;
					}
				}
			}
		}
	}
	if (Renames.IsEmpty())
	{
		return;
	}

	Builder.Reset();
	if (FMetasoundFrontendGraph* Graph = MutableDocument->RootGraph.FindGraph(BuildPageID))
	{
		for (FMetasoundFrontendNode& Node : Graph->Nodes)
		{
			if (const FGuid* NewID = Renames.Find(Node.GetID()))
			{
				Node.UpdateID(*NewID);
			}
		}
		for (FMetasoundFrontendEdge& Edge : Graph->Edges)
		{
			if (const FGuid* From = Renames.Find(Edge.FromNodeID))
			{
				Edge.FromNodeID = *From;
			}
			if (const FGuid* To = Renames.Find(Edge.ToNodeID))
			{
				Edge.ToNodeID = *To;
			}
		}
	}
	for (const TPair<FGuid, FGuid>& Rename : Renames)
	{
		InjectedNodeIds.Add(Rename.Value);
	}
	InjectedNodeIds.Sort([](const FGuid& A, const FGuid& B) { return A.ToString(EGuidFormats::Digits) < B.ToString(EGuidFormats::Digits); });
	Builder = MakeBuilder(*Copy);
}

FNormalizedDocument::~FNormalizedDocument()
{
	Builder.Reset();
	if (Copy.IsValid())
	{
		Copy->MarkAsGarbage();
	}
}

const FMetasoundFrontendDocument& FNormalizedDocument::GetDocument() const
{
	return Builder->GetConstDocumentChecked();
}
} // namespace UE::AssetDocument::Graph::Audio::MetaSound
