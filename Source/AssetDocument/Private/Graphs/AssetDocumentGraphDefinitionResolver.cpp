// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/AssetDocumentGraphDefinitionResolver.h"

#include "AssetDocumentJsonRegionUtils.h"
#include "Utils/AssetDocumentJsonKeys.h"

namespace
{
bool IsDefinitionId(const FString& Value)
{
	if (Value.IsEmpty())
	{
		return false;
	}

	const TCHAR First = Value[0];
	if (!FChar::IsAlpha(First) && First != TEXT('_'))
	{
		return false;
	}

	for (int32 Index = 1; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!FChar::IsAlnum(Character) &&
			Character != TEXT('_') &&
			Character != TEXT('.') &&
			Character != TEXT(':') &&
			Character != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

bool IsKnownDefinitionKind(const FString& Kind)
{
	return Kind == TEXT("DefinitionRef") ||
		Kind == TEXT("ClassRef") ||
		Kind == TEXT("AssetRef") ||
		Kind == TEXT("MemberRef") ||
		Kind == TEXT("PinType") ||
		Kind == TEXT("Literal");
}

class FResolverContext
{
public:
	FResolverContext(
		const TSharedPtr<FJsonObject>& InDefinitions,
		const FAssetDocumentGraphDefinitionResolveOptions& InOptions,
		FAssetDocumentGraphDefinitionResolveResult& InResult)
		: Definitions(InDefinitions)
		, Options(InOptions)
		, Result(InResult)
	{
	}

	void ValidateDefinitions()
	{
		if (!Definitions.IsValid())
		{
			return;
		}

		for (const auto& Pair : Definitions->Values)
		{
			const FString DefinitionPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Options.DefinitionsPath, FString(*Pair.Key));
			if (!IsDefinitionId(FString(*Pair.Key)))
			{
				Result.AddDiagnostic(
					TEXT("InvalidDefinitionId"),
					DefinitionPath,
					TEXT("Definition id must match ^[A-Za-z_][A-Za-z0-9_.:-]*$."));
				continue;
			}

			const TSharedPtr<FJsonObject> Object = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
			if (!Object.IsValid())
			{
				Result.AddDiagnostic(
					TEXT("UnknownDefinitionKind"),
					DefinitionPath,
					TEXT("Definition must be an object with a supported Kind."));
				continue;
			}

			FString Kind;
			if (!Object->TryGetStringField(TEXT("Kind"), Kind) || !IsKnownDefinitionKind(Kind))
			{
				Result.AddDiagnostic(
					TEXT("UnknownDefinitionKind"),
					FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(DefinitionPath, TEXT("Kind")),
					FString::Printf(TEXT("Unknown definition kind '%s'."), *Kind));
			}
		}
	}

	TSharedPtr<FJsonValue> ResolveValue(
		const TSharedPtr<FJsonValue>& Value,
		const FString& Path,
		TArray<FString>& Stack)
	{
		if (!Value.IsValid())
		{
			return nullptr;
		}

		switch (Value->Type)
		{
		case EJson::Object:
			return MakeShared<FJsonValueObject>(ResolveObject(Value->AsObject(), Path, Stack));
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> ResolvedItems;
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				ResolvedItems.Add(ResolveValue(Items[Index], FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(Path, Index), Stack));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(ResolvedItems));
		}
		default:
			return FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Value);
		}
	}

	TSharedPtr<FJsonObject> ResolveObject(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		TArray<FString>& Stack)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		FString Kind;
		if (Object->TryGetStringField(TEXT("Kind"), Kind))
		{
			if (!IsKnownDefinitionKind(Kind))
			{
				Result.AddDiagnostic(
					TEXT("UnknownDefinitionKind"),
					FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("Kind")),
					FString::Printf(TEXT("Unknown definition kind '%s'."), *Kind));
			}

			if (Kind == TEXT("DefinitionRef"))
			{
				FString Id;
				if (!Object->TryGetStringField(TEXT("Id"), Id) || Id.IsEmpty())
				{
					Result.AddDiagnostic(
						TEXT("UnresolvedDefinitionReference"),
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("Id")),
						TEXT("DefinitionRef requires a non-empty Id."));
					return FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Object);
				}

				const TSharedPtr<FJsonValue> Resolved = ResolveDefinition(Id, Path, Stack);
				return Resolved.IsValid() && Resolved->Type == EJson::Object
					? Resolved->AsObject()
					: FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Object);
			}
		}

		TSharedRef<FJsonObject> Resolved = MakeShared<FJsonObject>();
		TArray<FString> Keys;
		AssetDocumentServerJson::GetKeys(*Object, Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			Resolved->SetField(Key, ResolveValue(AssetDocumentServerJson::FindFieldRef(*Object, Key), FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, Key), Stack));
		}
		return Resolved;
	}

	TSharedPtr<FJsonValue> ResolveDefinition(
		const FString& Id,
		const FString& Path,
		TArray<FString>& Stack)
	{
		if (!Definitions.IsValid())
		{
			Result.AddDiagnostic(
				TEXT("UnresolvedDefinitionReference"),
				Path,
				FString::Printf(TEXT("Definition '%s' could not be resolved."), *Id));
			return nullptr;
		}

		const TSharedPtr<FJsonValue>* DefinitionValue = AssetDocumentServerJson::FindField(*Definitions, Id);
		if (!DefinitionValue || !DefinitionValue->IsValid())
		{
			Result.AddDiagnostic(
				TEXT("UnresolvedDefinitionReference"),
				Path,
				FString::Printf(TEXT("Definition '%s' could not be resolved."), *Id));
			return nullptr;
		}

		if (Stack.Contains(Id))
		{
			Result.AddDiagnostic(
				TEXT("CircularDefinitionReference"),
				Path,
				FString::Printf(TEXT("Definition '%s' participates in a circular reference."), *Id));
			return nullptr;
		}

		Stack.Add(Id);
		const TSharedPtr<FJsonValue> Resolved = ResolveValue(
			*DefinitionValue,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Options.DefinitionsPath, Id),
			Stack);
		Stack.Pop(EAllowShrinking::No);
		return Resolved;
	}

private:
	const TSharedPtr<FJsonObject>& Definitions;
	const FAssetDocumentGraphDefinitionResolveOptions& Options;
	FAssetDocumentGraphDefinitionResolveResult& Result;
};

FAssetDocumentGraphSpec ResolveGraph(
	const FAssetDocumentGraphSpec& Graph,
	const FString& GraphPath,
	FResolverContext& Context)
{
	FAssetDocumentGraphSpec Resolved = Graph;
	Resolved.Owner = nullptr;
	if (Graph.Owner.IsValid())
	{
		TArray<FString> Stack;
		Resolved.Owner = Context.ResolveObject(Graph.Owner, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Owner")), Stack);
	}

	Resolved.Signature = nullptr;
	if (Graph.Signature.IsValid())
	{
		TArray<FString> Stack;
		Resolved.Signature = Context.ResolveObject(Graph.Signature, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Signature")), Stack);
	}

	Resolved.Position = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Graph.Position);
	TArray<FString> GraphValueStack;
	Resolved.EntryPins = Context.ResolveValue(Graph.EntryPins, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("EntryPins")), GraphValueStack);
	GraphValueStack.Reset();
	Resolved.ResultPins = Context.ResolveValue(Graph.ResultPins, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("ResultPins")), GraphValueStack);
	GraphValueStack.Reset();
	Resolved.Metadata = Context.ResolveValue(Graph.Metadata, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Metadata")), GraphValueStack);
	GraphValueStack.Reset();
	Resolved.Evidence = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Graph.Evidence);

	Resolved.Nodes.Reset();
	for (int32 NodeIndex = 0; NodeIndex < Graph.Nodes.Num(); ++NodeIndex)
	{
		const FAssetDocumentNodeSpec& Node = Graph.Nodes[NodeIndex];
		const FString NodePath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Nodes")),
			NodeIndex);
		FAssetDocumentNodeSpec ResolvedNode = Node;
		ResolvedNode.Spawner = nullptr;
		if (Node.Spawner.IsValid())
		{
			TArray<FString> Stack;
			ResolvedNode.Spawner = Context.ResolveObject(Node.Spawner, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Spawner")), Stack);
		}

		ResolvedNode.Fields = nullptr;
		if (Node.Fields.IsValid())
		{
			TArray<FString> Stack;
			ResolvedNode.Fields = Context.ResolveObject(Node.Fields, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Fields")), Stack);
		}

		ResolvedNode.Pins = nullptr;
		if (Node.Pins.IsValid())
		{
			TArray<FString> Stack;
			ResolvedNode.Pins = Context.ResolveObject(Node.Pins, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Pins")), Stack);
		}

		ResolvedNode.Member = nullptr;
		if (Node.Member.IsValid())
		{
			TArray<FString> Stack;
			ResolvedNode.Member = Context.ResolveObject(Node.Member, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Member")), Stack);
		}
		ResolvedNode.Position = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Node.Position);
		ResolvedNode.SubgraphRefs = nullptr;
		if (Node.SubgraphRefs.IsValid())
		{
			TArray<FString> Stack;
			ResolvedNode.SubgraphRefs = Context.ResolveObject(Node.SubgraphRefs, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("SubgraphRefs")), Stack);
		}
		ResolvedNode.Evidence = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Node.Evidence);

		ResolvedNode.PinOverrides.Reset();
		for (int32 PinIndex = 0; PinIndex < Node.PinOverrides.Num(); ++PinIndex)
		{
			const FAssetDocumentPinOverrideSpec& PinOverride = Node.PinOverrides[PinIndex];
			const FString PinPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("PinOverrides")),
				PinIndex);
			FAssetDocumentPinOverrideSpec ResolvedPin = PinOverride;
			TArray<FString> Stack;
			ResolvedPin.Type = Context.ResolveValue(PinOverride.Type, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("Type")), Stack);
			Stack.Reset();
			ResolvedPin.DefaultValue = Context.ResolveValue(PinOverride.DefaultValue, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("DefaultValue")), Stack);
			Stack.Reset();
			ResolvedPin.DefaultObject = Context.ResolveValue(PinOverride.DefaultObject, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("DefaultObject")), Stack);
			Stack.Reset();
			ResolvedPin.DefaultTextValue =
				Context.ResolveValue(PinOverride.DefaultTextValue, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("DefaultTextValue")), Stack);
			ResolvedNode.PinOverrides.Add(MoveTemp(ResolvedPin));
		}

		Resolved.Nodes.Add(MoveTemp(ResolvedNode));
	}

	Resolved.Subgraphs.Reset();
	for (int32 SubgraphIndex = 0; SubgraphIndex < Graph.Subgraphs.Num(); ++SubgraphIndex)
	{
		Resolved.Subgraphs.Add(ResolveGraph(
			Graph.Subgraphs[SubgraphIndex],
			FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Subgraphs")),
				SubgraphIndex),
			Context));
	}

	return Resolved;
}
}

void FAssetDocumentGraphDefinitionResolveResult::AddDiagnostic(
	const FString& Code,
	const FString& Path,
	const FString& Message)
{
	FAssetDocumentGraphDiagnostic Diagnostic;
	Diagnostic.Code = Code;
	Diagnostic.Path = Path;
	Diagnostic.Message = Message;
	Diagnostics.Add(MoveTemp(Diagnostic));
}

FAssetDocumentGraphDefinitionResolveResult FAssetDocumentGraphDefinitionResolver::ResolveGraphArray(
	const TArray<FAssetDocumentGraphSpec>& Graphs,
	const TSharedPtr<FJsonObject>& Definitions,
	const FAssetDocumentGraphDefinitionResolveOptions& Options)
{
	FAssetDocumentGraphDefinitionResolveResult Result;
	FResolverContext Context(Definitions, Options, Result);
	Context.ValidateDefinitions();

	if (Definitions.IsValid())
	{
		for (const auto& Pair : Definitions->Values)
		{
			TArray<FString> Stack;
			Context.ResolveValue(Pair.Value, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Options.DefinitionsPath, FString(*Pair.Key)), Stack);
		}
	}

	for (int32 Index = 0; Index < Graphs.Num(); ++Index)
	{
		Result.Graphs.Add(ResolveGraph(
			Graphs[Index],
			FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(Options.GraphsPath, Index),
			Context));
	}

	return Result;
}
