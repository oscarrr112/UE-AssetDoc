// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Compile status of a MetaSound (design §5.1 / §5.2): the node messages the MetaSound editor's validation shows
// (FGraphBuilder::ValidateGraph), derived from public builder queries on every page of the normalised document,
// without registering anything:
//   - external / asset node whose class is not available (UMetasoundEditorGraphExternalNode::Validate
//     "Class definition ... not found"). The editor validates after registration, which auto-updates minor
//     versions, so a native class counts as available when its exact version or any minor version of the
//     same major is registered; an asset class when the MetaSound asset manager knows the asset;
//   - template node without its template, or with an interface the template cannot implement, or missing
//     required connections (INodeTemplate::HasRequiredConnections, e.g. an input template without a member);
//   - visible output node of a required interface output that is not connected
//     (UMetasoundEditorGraphOutputNode::Validate, e.g. OneShot "On Finished").
// Then, when the MetaSound is registered, the operator build check of its registered graph (CheckOperatorBuild,
// AssetDocumentMetaSoundBuildCheck.cpp; Source "Build", errors).
// {Status: UpToDate | UpToDateWithWarnings | Error, Messages[{Severity, Message, Graph, NodeGuid, NodeGuids?, Source}],
//  Registered}.

#include "AssetDocumentMetaSound.h"

#include "Dom/JsonObject.h"
#include "Interfaces/MetasoundFrontendInterfaceRegistry.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetKey.h"
#include "MetasoundAssetManager.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendNodeTemplateRegistry.h"
#include "MetasoundFrontendSearchEngine.h"

namespace UE::AssetDocument::Graph::Audio::MetaSound
{
namespace
{
struct FCompileMessage
{
	FString Severity;
	FString Message;
	FGuid NodeID;
	FGuid Page = Metasound::Frontend::DefaultPageID;
	FString Source = TEXT("Validation");
	TArray<FGuid> NodeIDs;
};

bool IsClassAvailable(const FMetasoundFrontendClassMetadata& Metadata)
{
	using namespace Metasound::Frontend;
	if (const IMetaSoundAssetManager* Manager = IMetaSoundAssetManager::Get())
	{
		if (Manager->IsAssetClass(Metadata))
		{
			return Manager->FindAssetPath(FMetaSoundAssetKey(Metadata)).IsValid();
		}
	}
	FMetasoundFrontendClass Registered;
	return FindRegisteredNodeClass(Metadata.GetClassName(), Metadata.GetVersion().Major, Registered);
}

void ValidateNode(const FMetaSoundFrontendDocumentBuilder& Builder, const FGuid& PageID, const FMetasoundFrontendNode& Node, TArray<FCompileMessage>& Out)
{
	using namespace Metasound::Frontend;
	const FMetasoundFrontendClass* Class = Builder.FindDependency(Node.ClassID);
	if (Class == nullptr)
	{
		Out.Add({TEXT("Error"), FString::Printf(TEXT("Node with ID '%s' class not found"), *Node.ClassID.ToString()), Node.GetID()});
		return;
	}
	const FMetasoundFrontendClassMetadata& Metadata = Class->Metadata;
	switch (Metadata.GetType())
	{
	case EMetasoundFrontendClassType::External:
	case EMetasoundFrontendClassType::Graph:
	{
		FClassInterfaceUpdates Updates;
		constexpr bool bUseHighestMinorVersion = false;
		if (!Builder.DiffAgainstRegistryInterface(Node.GetID(), PageID, bUseHighestMinorVersion, Updates) && !IsClassAvailable(Metadata))
		{
			Out.Add({TEXT("Error"), FString::Printf(TEXT("Class definition '%s %s (%s)' not found"),
				*Metadata.GetDisplayName().ToString(), *Metadata.GetVersion().ToString(), *Metadata.GetClassName().ToString()), Node.GetID()});
		}
		break;
	}
	case EMetasoundFrontendClassType::Template:
	{
		const INodeTemplate* Template = INodeTemplateRegistry::Get().FindTemplate(FNodeRegistryKey(Metadata));
		if (Template == nullptr)
		{
			Out.Add({TEXT("Error"), FString::Printf(TEXT("Template node interface missing for node class '%s'"), *Metadata.GetClassName().ToString()), Node.GetID()});
		}
		else if (!Template->IsValidNodeInterface(Node.Interface))
		{
			Out.Add({TEXT("Error"), FString::Printf(TEXT("Cannot implement template interface for node class '%s'"), *Metadata.GetClassName().ToString()), Node.GetID()});
		}
		else
		{
			FString Message;
			if (!Template->HasRequiredConnections(Builder, PageID, Node.GetID(), &Message))
			{
				Out.Add({TEXT("Warning"), Message, Node.GetID()});
			}
		}
		break;
	}
	default:
		break;
	}
}

void ValidateRequiredOutputs(const FMetaSoundFrontendDocumentBuilder& Builder, const FMetasoundFrontendGraph& Graph, const UClass& AssetClass, TArray<FCompileMessage>& Out)
{
	const TArray<FInterfaceFacts> Interfaces = DescribeInterfaces(Builder.GetConstDocumentChecked(), AssetClass);
	for (const FMetasoundFrontendNode& Node : Graph.Nodes)
	{
		const FMetasoundFrontendClass* Class = Builder.FindDependency(Node.ClassID);
		// Only output nodes the editor graph shows (an output node without a location has no editor node).
		if (Class == nullptr || Class->Metadata.GetType() != EMetasoundFrontendClassType::Output
			|| Node.Style.Display.Locations.IsEmpty() || Node.Interface.Inputs.IsEmpty())
		{
			continue;
		}
		for (const FInterfaceFacts& Facts : Interfaces)
		{
			if (!Facts.Definition.IsSet())
			{
				continue;
			}
			FMetasoundFrontendInterface Definition = Facts.Definition.GetValue();
			FText RequiredText;
			if (Definition.Outputs.ContainsByPredicate([&Node](const FMetasoundFrontendClassOutput& Output) { return Output.Name == Node.Name; })
				&& Definition.IsMemberOutputRequired(Node.Name, RequiredText)
				&& !Builder.IsNodeInputConnected(Node.GetID(), Node.Interface.Inputs.Last().VertexID, &Graph.PageID))
			{
				Out.Add({TEXT("Warning"), RequiredText.ToString(), Node.GetID()});
			}
		}
	}
}
FGuid ValidationErrorNodeForTest;
} // namespace

void SetValidationErrorNodeForTest(const FGuid& NodeID)
{
	ValidationErrorNodeForTest = NodeID;
}

TArray<FString> GetMessageNodeIds(const FJsonObject& Row)
{
	TArray<FString> Ids;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Row.TryGetArrayField(TEXT("NodeGuids"), Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Ids.Add(Value->AsString());
		}
	}
	FString First;
	if (Ids.IsEmpty() && Row.TryGetStringField(TEXT("NodeGuid"), First) && !First.IsEmpty())
	{
		Ids.Add(First);
	}
	return Ids;
}

TSharedPtr<FJsonObject> DescribeCompileStatus(const UObject& Asset)
{
	if (AsMetaSound(Asset) == nullptr)
	{
		return nullptr;
	}
	const FNormalizedDocument Normalized(Asset);
	if (!Normalized.IsValid())
	{
		return nullptr;
	}
	const FMetaSoundFrontendDocumentBuilder& Builder = Normalized.GetBuilder();
	const FMetasoundFrontendGraphClass& Root = Normalized.GetDocument().RootGraph;
	TArray<FCompileMessage> Messages;
	// A preset's graph is rebuilt from its parent on registration; the editor shows it read-only.
	if (!Builder.IsPreset())
	{
		// Every page, as the editor validates the page a user focuses (FGraphBuilder::ValidateGraph on the build page).
		for (const FGuid& PageID : GetOrderedPageIDs(Root))
		{
			const FMetasoundFrontendGraph& Graph = Root.FindConstGraphChecked(PageID);
			const int32 First = Messages.Num();
			for (const FMetasoundFrontendNode& Node : Graph.Nodes)
			{
				ValidateNode(Builder, PageID, Node, Messages);
				if (ValidationErrorNodeForTest.IsValid() && Node.GetID() == ValidationErrorNodeForTest)
				{
					Messages.Add({TEXT("Error"), TEXT("Test validation error"), Node.GetID()});
				}
			}
			ValidateRequiredOutputs(Builder, Graph, *Asset.GetClass(), Messages);
			for (int32 Index = First; Index < Messages.Num(); ++Index)
			{
				Messages[Index].Page = PageID;
			}
		}
		TArray<FBuildMessage> Build;
		if (CheckOperatorBuild(Asset, Build))
		{
			const FGuid Registered = GetRegisteredPageID(Root);
			for (FBuildMessage& Message : Build)
			{
				FCompileMessage& Row = Messages.Add_GetRef({TEXT("Error"), MoveTemp(Message.Message), Message.NodeIDs.IsEmpty() ? FGuid() : Message.NodeIDs[0]});
				Row.NodeIDs = MoveTemp(Message.NodeIDs);
				Row.Page = Registered;
				Row.Source = TEXT("Build");
			}
		}
	}

	bool bError = false;
	bool bWarning = false;
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FCompileMessage& Message : Messages)
	{
		bError |= Message.Severity == TEXT("Error");
		bWarning |= Message.Severity == TEXT("Warning");
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Severity"), Message.Severity);
		Row->SetStringField(TEXT("Message"), Message.Message);
		Row->SetStringField(TEXT("Graph"), GetPageName(Message.Page));
		Row->SetStringField(TEXT("NodeGuid"), Message.NodeID.IsValid() ? Message.NodeID.ToString(EGuidFormats::Digits) : FString());
		Row->SetStringField(TEXT("Source"), Message.Source);
		if (!Message.NodeIDs.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Ids;
			for (const FGuid& Id : Message.NodeIDs)
			{
				Ids.Add(MakeShared<FJsonValueString>(Id.ToString(EGuidFormats::Digits)));
			}
			Row->SetArrayField(TEXT("NodeGuids"), Ids);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	Compile->SetStringField(TEXT("Status"), bError ? TEXT("Error") : bWarning ? TEXT("UpToDateWithWarnings") : TEXT("UpToDate"));
	Compile->SetArrayField(TEXT("Messages"), Rows);
	bool bRegistered = false;
	if (const Metasound::Frontend::IMetaSoundAssetManager* Manager = Metasound::Frontend::IMetaSoundAssetManager::Get())
	{
		if (const FMetasoundAssetBase* MetaSound = Manager->GetAsAsset(Asset))
		{
			bRegistered = MetaSound->IsRegistered();
		}
	}
	Compile->SetBoolField(TEXT("Registered"), bRegistered);
	return Compile;
}
} // namespace UE::AssetDocument::Graph::Audio::MetaSound
