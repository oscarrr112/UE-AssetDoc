// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Operator build check of a MetaSound (design §5.1 step 3, phase M3). The MetaSound editor validates its graph
// view; a graph can still fail to become an operator (a cycle through a variable's Get / Set, duplicate inputs, a
// connection whose data types the node does not accept), which a human only sees in the log when pressing Play. The
// check builds the graph the frontend registered (INodeClassRegistry::GetGraph: the document the registration
// prepared, for the page it resolved) the way a generator does (GeneratorBuilder::BuildGraphOperator): inputs bound
// to their default literals, an operator builder with every validation and no pruning, and the environment a test
// generator gets (no audio device: audio bus nodes only warn). Build errors map to the frontend node IDs they name
// (INode::GetInstanceID is the frontend node ID).

#include "AssetDocumentMetaSound.h"

#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "MetasoundBuildError.h"
#include "MetasoundBuilderInterface.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEnvironment.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "MetasoundGraph.h"
#include "MetasoundOperatorBuilder.h"
#include "MetasoundOperatorBuilderSettings.h"
#include "MetasoundOperatorSettings.h"
#include "MetasoundVertexData.h"

namespace UE::AssetDocument::Graph::Audio::MetaSound
{
namespace
{
Metasound::FMetasoundEnvironment MakeBuildEnvironment(const UObject& Asset)
{
	using namespace Metasound;
	FMetasoundEnvironment Environment;
	Environment.SetValue<uint64>(CoreInterface::Environment::InstanceID, 0);
	Environment.SetValue<TArray<FGuid>>(CoreInterface::Environment::GraphHierarchy, TArray<FGuid>({FGuid()}));
	Environment.SetValue<uint32>(Frontend::SourceInterface::Environment::SoundUniqueID, 0);
	Environment.SetValue<bool>(Frontend::SourceInterface::Environment::IsPreview, true);
	Environment.SetValue<uint64>(Frontend::SourceInterface::Environment::TransmitterID, 0);
	Environment.SetValue<FString>(Frontend::SourceInterface::Environment::GraphName, Asset.GetPathName());
	return Environment;
}

void AddNode(const Metasound::INode* Node, TArray<FGuid>& Out)
{
	if (Node != nullptr && Node->GetInstanceID().IsValid())
	{
		Out.AddUnique(Node->GetInstanceID());
	}
}
} // namespace

bool CheckOperatorBuild(const UObject& Asset, TArray<FBuildMessage>& OutMessages)
{
	using namespace Metasound;
	using namespace Metasound::Frontend;
	const IMetaSoundAssetManager* Manager = IMetaSoundAssetManager::Get();
	const FMetasoundAssetBase* MetaSound = Manager != nullptr ? Manager->GetAsAsset(Asset) : nullptr;
	INodeClassRegistry* Registry = INodeClassRegistry::Get();
	if (MetaSound == nullptr || Registry == nullptr || !MetaSound->IsRegistered())
	{
		return false;
	}
	// Registration registers no graph (a null one) when a node class the document uses is not registered (validation
	// reports it); GetGraph would log an error for it.
	const IMetaSoundDocumentInterface* DocumentInterface = AsMetaSound(Asset);
	if (DocumentInterface == nullptr)
	{
		return false;
	}
	for (const FMetasoundFrontendClass& Dependency : DocumentInterface->GetConstDocument().Dependencies)
	{
		if (Dependency.Metadata.GetType() == EMetasoundFrontendClassType::External && !Registry->IsNodeRegistered(FNodeClassRegistryKey(Dependency.Metadata)))
		{
			return false;
		}
	}
	const FGraphRegistryKey Key = MetaSound->GetGraphRegistryKey();
	if (!Registry->IsGraphRegistered(Key))
	{
		return false;
	}
	const TSharedPtr<const FGraph> Graph = Registry->GetGraph(Key);
	if (!Graph.IsValid())
	{
		return false;
	}

	// The block rate does not change what builds; a fixed rate keeps the check free of quality-settings lookups.
	const FOperatorSettings OperatorSettings(48000, 100.f);
	const FInputVertexInterface& InputInterface = Graph->GetVertexInterface().GetInputInterface();
	FInputVertexInterfaceData InputData(InputInterface);
	IDataTypeRegistry& DataTypes = IDataTypeRegistry::Get();
	for (const FInputDataVertex& Vertex : InputInterface)
	{
		const EDataReferenceAccessType Access = Vertex.AccessType == EVertexAccessType::Value ? EDataReferenceAccessType::Value : EDataReferenceAccessType::Write;
		TOptional<FAnyDataReference> Reference = DataTypes.CreateDataReference(Vertex.DataTypeName, Access, Vertex.GetDefaultLiteral(), OperatorSettings);
		if (Reference.IsSet())
		{
			InputData.BindVertex(Vertex.VertexName, *Reference);
		}
	}
	const FMetasoundEnvironment Environment = MakeBuildEnvironment(Asset);
	// Every validation, no pruning, and all errors collected rather than stopping at the first.
	const FOperatorBuilderSettings& Settings = FOperatorBuilderSettings::GetDefaultDebugSettings();
	const FBuildGraphOperatorParams Params(*Graph, OperatorSettings, InputData, Environment);
	FBuildResults Results;
	const TUniquePtr<IOperator> Operator = FOperatorBuilder(Settings).BuildGraphOperator(Params, Results);

	for (const TUniquePtr<IOperatorBuildError>& Error : Results.Errors)
	{
		if (!Error.IsValid() || Error->GetErrorType() == FNodePrunedError::ErrorType)
		{
			continue;
		}
		FBuildMessage& Message = OutMessages.AddDefaulted_GetRef();
		const FString Description = Error->GetErrorDescription().ToString();
		Message.Message = FString::Printf(TEXT("The graph does not build into an operator (%s)%s%s"), *Error->GetErrorType().ToString(),
			Description.IsEmpty() ? TEXT("") : TEXT(": "), *Description);
		for (const INode* Node : Error->GetNodes())
		{
			AddNode(Node, Message.NodeIDs);
		}
		for (const FDataEdge& Edge : Error->GetDataEdges())
		{
			AddNode(Edge.To.Node, Message.NodeIDs);
			AddNode(Edge.From.Node, Message.NodeIDs);
		}
		for (const FInputDataDestination& Destination : Error->GetInputDataDestinations())
		{
			AddNode(Destination.Node, Message.NodeIDs);
		}
		for (const FOutputDataSource& Source : Error->GetOutputDataSources())
		{
			AddNode(Source.Node, Message.NodeIDs);
		}
	}
	if (!Operator.IsValid() && OutMessages.IsEmpty())
	{
		OutMessages.AddDefaulted_GetRef().Message = TEXT("The graph does not build into an operator.");
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Audio::MetaSound
