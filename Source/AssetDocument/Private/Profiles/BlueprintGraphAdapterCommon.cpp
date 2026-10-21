// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Profiles/BlueprintGraphAdapterCommon.h"

#include "AssetDocumentJsonRegionUtils.h"
#include "Graphs/AssetDocumentGraphDefinitionResolver.h"
#include "Graphs/AssetDocumentGraphParser.h"

#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/AssetDocumentJsonKeys.h"

const TArray<FAssetDocumentBlueprintGraphRegion>& FAssetDocumentBlueprintGraphAdapterCommon::GetGraphRegions(
	bool bIncludeDelegateSignatureGraphs)
{
	static const TArray<FAssetDocumentBlueprintGraphRegion> BlueprintRegions = {
		{TEXT("UbergraphPages"), TEXT("/Body/UbergraphPages"), EAssetDocumentK2GraphRegion::UbergraphPages},
		{TEXT("FunctionGraphs"), TEXT("/Body/FunctionGraphs"), EAssetDocumentK2GraphRegion::FunctionGraphs},
		{TEXT("MacroGraphs"), TEXT("/Body/MacroGraphs"), EAssetDocumentK2GraphRegion::MacroGraphs},
	};
	static const TArray<FAssetDocumentBlueprintGraphRegion> AnimBlueprintRegions = {
		{TEXT("UbergraphPages"), TEXT("/Body/UbergraphPages"), EAssetDocumentK2GraphRegion::UbergraphPages},
		{TEXT("FunctionGraphs"), TEXT("/Body/FunctionGraphs"), EAssetDocumentK2GraphRegion::FunctionGraphs},
		{TEXT("MacroGraphs"), TEXT("/Body/MacroGraphs"), EAssetDocumentK2GraphRegion::MacroGraphs},
		{TEXT("DelegateSignatureGraphs"), TEXT("/Body/DelegateSignatureGraphs"), EAssetDocumentK2GraphRegion::DelegateSignatureGraphs},
	};
	return bIncludeDelegateSignatureGraphs ? AnimBlueprintRegions : BlueprintRegions;
}

bool FAssetDocumentBlueprintGraphAdapterCommon::IsEmptyFunctionOrMacroNoop(
	const FAssetDocumentBlueprintGraphRegion& Region,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs)
{
	return DesiredGraphs.IsEmpty()
		&& (Region.K2Region == EAssetDocumentK2GraphRegion::FunctionGraphs
			|| Region.K2Region == EAssetDocumentK2GraphRegion::MacroGraphs
			|| Region.K2Region == EAssetDocumentK2GraphRegion::DelegateSignatureGraphs);
}

FAssetDocumentCapabilityResult FAssetDocumentBlueprintGraphAdapterCommon::Failure(
	const FString& Message,
	const FString& Path,
	const FString& Code)
{
	return FAssetDocumentCapabilityResult::Failure(Message, Path, Code);
}

FAssetDocumentCapabilityResult FAssetDocumentBlueprintGraphAdapterCommon::InvalidRegionTypeFailure(
	const FAssetDocumentBlueprintGraphRegion& Region)
{
	return Failure(
		FString::Printf(TEXT("Body.%s must be an array when authored"), *Region.Name),
		Region.Path,
		TEXT("InvalidGraphRegionType"));
}

FAssetDocumentCapabilityResult FAssetDocumentBlueprintGraphAdapterCommon::GraphDiagnosticsFailure(
	const TArray<FAssetDocumentGraphDiagnostic>& Diagnostics,
	const FString& FallbackPath)
{
	if (Diagnostics.IsEmpty())
	{
		return Failure(TEXT("Graph region validation failed"), FallbackPath, TEXT("InvalidGraphRegion"));
	}

	FAssetDocumentCapabilityResult Result = FAssetDocumentCapabilityResult::Failure(
		Diagnostics[0].Message,
		Diagnostics[0].Path,
		Diagnostics[0].Code);
	Result.Diagnostics.Reset();
	for (const FAssetDocumentGraphDiagnostic& GraphDiagnostic : Diagnostics)
	{
		FAssetDocumentDiagnostic Diagnostic;
		Diagnostic.Path = GraphDiagnostic.Path;
		Diagnostic.Code = GraphDiagnostic.Code;
		Diagnostic.Message = GraphDiagnostic.Message;
		Result.Diagnostics.Add(MoveTemp(Diagnostic));
	}
	return Result;
}

FString FAssetDocumentBlueprintGraphAdapterCommon::MakeNodePath(
	const FAssetDocumentBlueprintGraphRegion& Region,
	int32 GraphIndex,
	int32 NodeIndex)
{
	return FString::Printf(TEXT("%s/%d/Nodes/%d"), *Region.Path, GraphIndex, NodeIndex);
}

UClass* FAssetDocumentBlueprintGraphAdapterCommon::ResolveNodeClass(const FString& ClassPath)
{
	return ClassPath.IsEmpty() ? nullptr : StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
}

FAssetDocumentUnsupportedNodeDiagnostic FAssetDocumentBlueprintGraphAdapterCommon::MakeUnsupportedNodeDiagnostic(
	const FAssetDocumentNodeSpec& Node,
	const FString& Path,
	const FString& Code,
	const FString& Reason,
	const FString& SuggestedAction)
{
	FAssetDocumentUnsupportedNodeDiagnostic Diagnostic;
	Diagnostic.Code = Code;
	Diagnostic.Path = Path;
	Diagnostic.Class = Node.Class;
	Diagnostic.Capability = Node.Capability;
	Diagnostic.Member = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Node.Member);
	Diagnostic.Reason = Reason;
	Diagnostic.SuggestedAction = SuggestedAction;
	return Diagnostic;
}

FAssetDocumentCapabilityResult FAssetDocumentBlueprintGraphAdapterCommon::UnsupportedGraphFailure(
	const FString& FallbackPath,
	const TArray<FAssetDocumentUnsupportedNodeDiagnostic>& UnsupportedDiagnostics)
{
	if (UnsupportedDiagnostics.IsEmpty())
	{
		return Failure(TEXT("Graph region validation failed"), FallbackPath, TEXT("InvalidGraphRegion"));
	}

	const FAssetDocumentUnsupportedNodeDiagnostic& FirstUnsupported = UnsupportedDiagnostics[0];
	FAssetDocumentCapabilityResult Result = FAssetDocumentCapabilityResult::Failure(
		FString::Printf(TEXT("%s at %s: %s"), *FirstUnsupported.Code, *FirstUnsupported.Path, *FirstUnsupported.Reason),
		FirstUnsupported.Path,
		FirstUnsupported.Code);
	Result.Diagnostics.Reset();

	TArray<TSharedPtr<FJsonValue>> UnsupportedPayload;
	for (const FAssetDocumentUnsupportedNodeDiagnostic& Unsupported : UnsupportedDiagnostics)
	{
		FAssetDocumentDiagnostic Diagnostic;
		Diagnostic.Path = Unsupported.Path;
		Diagnostic.Code = Unsupported.Code;
		Diagnostic.Message = FString::Printf(TEXT("%s at %s: %s"), *Unsupported.Code, *Unsupported.Path, *Unsupported.Reason);
		Result.Diagnostics.Add(MoveTemp(Diagnostic));
		UnsupportedPayload.Add(MakeShared<FJsonValueObject>(Unsupported.ToJsonObject()));
	}
	Result.Payload = MakeShared<FJsonObject>();
	Result.Payload->SetArrayField(TEXT("UnsupportedGraphDiagnostics"), MoveTemp(UnsupportedPayload));
	return Result;
}

FAssetDocumentCapabilityResult FAssetDocumentBlueprintGraphAdapterCommon::ParseDesiredGraphRegion(
	const TSharedRef<FJsonObject>& DesiredBody,
	const FAssetDocumentBlueprintGraphRegion& Region,
	TArray<FAssetDocumentGraphSpec>& OutGraphs)
{
	OutGraphs.Reset();
	const TSharedPtr<FJsonValue>* RegionValue = AssetDocumentServerJson::FindField(*DesiredBody, Region.Name);
	if (!RegionValue || !RegionValue->IsValid() || (*RegionValue)->Type == EJson::Null)
	{
		return FAssetDocumentCapabilityResult::Success();
	}
	if ((*RegionValue)->Type != EJson::Array)
	{
		return InvalidRegionTypeFailure(Region);
	}

	FAssetDocumentGraphParseOptions ParseOptions;
	ParseOptions.Path = Region.Path;
	FAssetDocumentGraphParseResult ParseResult =
		FAssetDocumentGraphParser::ParseGraphArray((*RegionValue)->AsArray(), ParseOptions);
	if (!ParseResult.IsValid())
	{
		return GraphDiagnosticsFailure(ParseResult.Diagnostics, Region.Path);
	}

	OutGraphs = MoveTemp(ParseResult.Graphs);
	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult FAssetDocumentBlueprintGraphAdapterCommon::ParseAndResolveDesiredGraphRegion(
	const FAssetDocumentCapabilityContext& Context,
	const TSharedRef<FJsonObject>& DesiredBody,
	const FAssetDocumentBlueprintGraphRegion& Region,
	TArray<FAssetDocumentGraphSpec>& OutGraphs)
{
	TArray<FAssetDocumentGraphSpec> ParsedGraphs;
	const FAssetDocumentCapabilityResult ParseResult = ParseDesiredGraphRegion(DesiredBody, Region, ParsedGraphs);
	if (!ParseResult.bSuccess)
	{
		return ParseResult;
	}

	FAssetDocumentGraphDefinitionResolveOptions ResolveOptions;
	ResolveOptions.GraphsPath = Region.Path;
	const TSharedPtr<FJsonObject> Definitions = Context.Definitions ? *Context.Definitions : nullptr;
	FAssetDocumentGraphDefinitionResolveResult ResolveResult =
		FAssetDocumentGraphDefinitionResolver::ResolveGraphArray(ParsedGraphs, Definitions, ResolveOptions);
	if (!ResolveResult.IsValid())
	{
		return GraphDiagnosticsFailure(ResolveResult.Diagnostics, Region.Path);
	}

	OutGraphs = MoveTemp(ResolveResult.Graphs);
	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult FAssetDocumentBlueprintGraphAdapterCommon::ValidateSupportedNodeClasses(
	const FAssetDocumentBlueprintGraphRegion& Region,
	const TArray<FAssetDocumentGraphSpec>& Graphs)
{
	const FAssetDocumentNodeAdapterRegistry CurrentTierRegistry =
		FAssetDocumentK2GraphAdapter::CreateTier1NodeAdapterRegistry();
	TArray<FAssetDocumentUnsupportedNodeDiagnostic> UnsupportedDiagnostics;
	for (int32 GraphIndex = 0; GraphIndex < Graphs.Num(); ++GraphIndex)
	{
		const FAssetDocumentGraphSpec& Graph = Graphs[GraphIndex];
		for (int32 NodeIndex = 0; NodeIndex < Graph.Nodes.Num(); ++NodeIndex)
		{
			const FAssetDocumentNodeSpec& Node = Graph.Nodes[NodeIndex];
			const FString Path = MakeNodePath(Region, GraphIndex, NodeIndex);
			UClass* NodeClass = ResolveNodeClass(Node.Class);
			if (!NodeClass)
			{
				UnsupportedDiagnostics.Add(MakeUnsupportedNodeDiagnostic(
					Node,
					Path,
					TEXT("UnresolvedGraphNodeClass"),
					FString::Printf(TEXT("graph node class '%s' could not be loaded"), *Node.Class),
					TEXT("fix the node Class path or remove the node from the managed graph")));
				continue;
			}

			if (CurrentTierRegistry.FindAdapter(NodeClass).IsValid())
			{
				continue;
			}

			UnsupportedDiagnostics.Add(MakeUnsupportedNodeDiagnostic(
				Node,
				Path,
				TEXT("UnsupportedGraphNodeClass"),
				TEXT("node class has no registered AssetDocument adapter in the current tier"),
				TEXT("add a thin node adapter for this class or remove the node from the managed graph")));
		}
	}

	return UnsupportedDiagnostics.IsEmpty()
		? FAssetDocumentCapabilityResult::Success()
		: UnsupportedGraphFailure(Region.Path, UnsupportedDiagnostics);
}

void FAssetDocumentBlueprintGraphAdapterCommon::MergeSkippedGraphEvidence(
	TSharedRef<FJsonObject>& OutBodyJson,
	const TArray<TSharedPtr<FJsonValue>>& SkippedNodes)
{
	if (SkippedNodes.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Skipped;
	const TSharedPtr<FJsonObject>* ExistingSkipped = nullptr;
	if (OutBodyJson->TryGetObjectField(TEXT("_Skipped"), ExistingSkipped)
		&& ExistingSkipped
		&& ExistingSkipped->IsValid())
	{
		Skipped = *ExistingSkipped;
	}
	else
	{
		Skipped = MakeShared<FJsonObject>();
		OutBodyJson->SetObjectField(TEXT("_Skipped"), Skipped);
	}

	TSharedPtr<FJsonObject> Graphs;
	const TSharedPtr<FJsonObject>* ExistingGraphs = nullptr;
	if (Skipped->TryGetObjectField(TEXT("Graphs"), ExistingGraphs)
		&& ExistingGraphs
		&& ExistingGraphs->IsValid())
	{
		Graphs = *ExistingGraphs;
	}
	else
	{
		Graphs = MakeShared<FJsonObject>();
		Skipped->SetObjectField(TEXT("Graphs"), Graphs);
	}

	TArray<TSharedPtr<FJsonValue>> Nodes = SkippedNodes;
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	if (Graphs->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes)
	{
		Nodes.Append(*ExistingNodes);
	}
	Graphs->SetStringField(TEXT("Reason"), TEXT("UnsupportedGraphNodeClass"));
	Graphs->SetNumberField(TEXT("Count"), Nodes.Num());
	Graphs->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
}

TSharedRef<FJsonObject> FAssetDocumentBlueprintGraphAdapterCommon::MakeRawCapabilityDiffEntry(
	const FString& Path,
	const FString& Status,
	const TSharedPtr<FJsonValue>& Current,
	const TSharedPtr<FJsonValue>& Desired,
	const FString& Change,
	const FString& Message,
	const FString& Code)
{
	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("path"), Path);
	Entry->SetStringField(TEXT("status"), Status);
	if (!Change.IsEmpty())
	{
		Entry->SetStringField(TEXT("change"), Change);
	}
	if (!Message.IsEmpty())
	{
		Entry->SetStringField(TEXT("message"), Message);
	}
	if (!Code.IsEmpty())
	{
		Entry->SetStringField(TEXT("code"), Code);
	}
	Entry->SetField(
		TEXT("current"),
		Current.IsValid()
			? FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Current)
			: MakeShared<FJsonValueNull>());
	Entry->SetField(
		TEXT("desired"),
		Desired.IsValid()
			? FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Desired)
			: MakeShared<FJsonValueNull>());
	return Entry;
}

FString FAssetDocumentBlueprintGraphAdapterCommon::RewriteGraphDiffPath(
	const FString& Path,
	const FAssetDocumentBlueprintGraphRegion& Region)
{
	const FString UBlueprintPrefix = TEXT("/Body/UbergraphPages");
	if (Region.Path == UBlueprintPrefix || !Path.StartsWith(UBlueprintPrefix))
	{
		return Path;
	}
	return Region.Path + Path.RightChop(FCString::Strlen(*UBlueprintPrefix));
}

TSharedRef<FJsonObject> FAssetDocumentBlueprintGraphAdapterCommon::MakeCapabilityDiffEntry(
	const FAssetDocumentBlueprintGraphRegion& Region,
	const FAssetDocumentGraphDiffEntry& GraphEntry)
{
	const bool bUnsupported = GraphEntry.Status == TEXT("unsupported");
	const bool bUnchanged = GraphEntry.Status == TEXT("unchanged");
	const FString PublicStatus = bUnsupported
		? FString(TEXT("skipped"))
		: (bUnchanged ? FString(TEXT("unchanged")) : FString(TEXT("changed")));
	const FString Change = bUnchanged ? FString() : GraphEntry.Status;
	return MakeRawCapabilityDiffEntry(
		RewriteGraphDiffPath(GraphEntry.Path, Region),
		PublicStatus,
		GraphEntry.Current,
		GraphEntry.Desired,
		Change,
		GraphEntry.Message,
		bUnsupported ? FString(TEXT("UnsupportedGraphDiff")) : FString());
}

FString FAssetDocumentBlueprintGraphAdapterCommon::MakeSkippedGraphPath(
	const FAssetDocumentBlueprintGraphRegion& Region,
	const TSharedPtr<FJsonObject>& SkippedNode)
{
	FString GraphName;
	if (SkippedNode.IsValid()
		&& SkippedNode->TryGetStringField(TEXT("Graph"), GraphName)
		&& !GraphName.IsEmpty())
	{
		return FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Region.Path, GraphName);
	}
	return Region.Path;
}

FString FAssetDocumentBlueprintGraphAdapterCommon::MakeSkippedNodeMessage(
	const TSharedPtr<FJsonObject>& SkippedNode)
{
	FString ClassPath;
	FString NodeTitle;
	if (SkippedNode.IsValid())
	{
		SkippedNode->TryGetStringField(TEXT("Class"), ClassPath);
		SkippedNode->TryGetStringField(TEXT("NodeTitle"), NodeTitle);
	}
	return FString::Printf(
		TEXT("current asset contains unsupported graph node%s%s; graph content cannot be fully canonicalized"),
		ClassPath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" class '%s'"), *ClassPath),
		NodeTitle.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" titled '%s'"), *NodeTitle));
}

void FAssetDocumentBlueprintGraphAdapterCommon::AppendSkippedGraphDiffEntries(
	const FAssetDocumentBlueprintGraphRegion& Region,
	const TArray<TSharedPtr<FJsonValue>>& SkippedNodes,
	TArray<TSharedPtr<FJsonValue>>& OutDiffEntries)
{
	for (const TSharedPtr<FJsonValue>& SkippedValue : SkippedNodes)
	{
		const TSharedPtr<FJsonObject> SkippedNode =
			SkippedValue.IsValid() ? SkippedValue->AsObject() : nullptr;
		FString Code = TEXT("UnsupportedGraphNodeClass");
		if (SkippedNode.IsValid())
		{
			SkippedNode->TryGetStringField(TEXT("Reason"), Code);
		}
		OutDiffEntries.Add(MakeShared<FJsonValueObject>(MakeRawCapabilityDiffEntry(
			MakeSkippedGraphPath(Region, SkippedNode),
			TEXT("skipped"),
			SkippedValue,
			nullptr,
			TEXT("unsupported"),
			MakeSkippedNodeMessage(SkippedNode),
			Code)));
	}
}
