// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Graphs/K2NodeAdapters/AssetDocumentK2NodeSpawnerResolver.h"

#include "Graphs/K2NodeAdapters/AssetDocumentK2MemberRefUtils.h"

#include "BlueprintEventNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_Self.h"
#include "K2Node_Variable.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
FAssetDocumentCapabilityResult SpawnerFailure(
	const FAssetDocumentNodeApplyContext& Context,
	const FAssetDocumentNodeSpec& NodeSpec,
	const FString& Message,
	const FString& Code,
	bool bClassPath = false)
{
	return FAssetDocumentCapabilityResult::Failure(
		Message,
		bClassPath ? Context.NodePath / TEXT("Class") : Context.NodePath / TEXT("Member"),
		Code);
}

bool IsUbergraphPage(const UBlueprint* Blueprint, const UEdGraph* Graph)
{
	return Blueprint && Graph && Blueprint->UbergraphPages.Contains(Graph);
}

}

FAssetDocumentCapabilityResult FAssetDocumentK2NodeSpawnerResolver::Resolve(
	const FAssetDocumentNodeApplyContext& Context,
	UEdGraph* Graph,
	const FAssetDocumentNodeSpec& NodeSpec,
	bool bRequireUEPlacementAdmission,
	FAssetDocumentK2ResolvedNodeSpawner& OutResolved)
{
	OutResolved.Spawner.Reset();
	OutResolved.ExpectedNodeClass = nullptr;

	UClass* NodeClass = NodeSpec.Class.IsEmpty()
		? nullptr
		: StaticLoadClass(UEdGraphNode::StaticClass(), nullptr, *NodeSpec.Class);
	if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
	{
		return SpawnerFailure(
			Context,
			NodeSpec,
			FString::Printf(TEXT("Graph node class '%s' could not be loaded"), *NodeSpec.Class),
			TEXT("UnresolvedGraphNodeClass"),
			true);
	}

	UBlueprintNodeSpawner* Spawner = nullptr;
	if (NodeClass->IsChildOf(UK2Node_Event::StaticClass()))
	{
		if (!IsUbergraphPage(Context.Blueprint, Graph))
		{
			return SpawnerFailure(
				Context,
				NodeSpec,
				FString::Printf(TEXT("Graph event node '%s' can only be placed in an Ubergraph page"), *NodeSpec.Id),
				TEXT("UnsupportedGraphEventPlacement"));
		}

		UFunction* Function = FAssetDocumentK2MemberRefUtils::ResolveMemberFunction(Context.Blueprint, NodeSpec.Member);
		if (!Function)
		{
			return NodeSpec.Member.IsValid()
				? FAssetDocumentK2MemberRefUtils::MakeUnresolvedMemberFailure(Context, NodeSpec, TEXT("UnresolvedGraphMemberReference"))
				: FAssetDocumentK2MemberRefUtils::MakeMissingMemberFailure(Context, NodeSpec, TEXT("MissingGraphMemberReference"));
		}
		if (!UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function))
		{
			return SpawnerFailure(
				Context,
				NodeSpec,
				FString::Printf(TEXT("Graph function '%s' cannot be placed as an event"), *Function->GetPathName()),
				TEXT("UnsupportedGraphEvent"));
		}
		if (!Context.Blueprint->AllowFunctionOverride(Function))
		{
			return SpawnerFailure(
				Context,
				NodeSpec,
				FString::Printf(
					TEXT("Graph event function '%s' is excluded by Blueprint '%s'"),
					*Function->GetPathName(),
					*Context.Blueprint->GetPathName()),
				TEXT("UnsupportedGraphEvent"));
		}

		UBlueprintEventNodeSpawner* EventSpawner = UBlueprintEventNodeSpawner::Create(Function);
		if (bRequireUEPlacementAdmission && EventSpawner)
		{
			IBlueprintNodeBinder::FBindingSet EmptyBindings;
			const UK2Node_Event* ExistingEvent = EventSpawner->FindPreExistingEvent(Context.Blueprint, EmptyBindings);
			if (ExistingEvent && ExistingEvent->GetGraph() != Graph)
			{
				return FAssetDocumentCapabilityResult::Failure(
					FString::Printf(TEXT("Graph event node '%s' is already owned by a different Ubergraph page"), *NodeSpec.Id),
					Context.NodePath,
					TEXT("GraphEventOwnedByDifferentUbergraph"));
			}
			if (!ExistingEvent || ExistingEvent->IsAutomaticallyPlacedGhostNode())
			{
				const UK2Node_Event* EventTemplate = Cast<UK2Node_Event>(EventSpawner->GetTemplateNode(Graph));
				if (!EventTemplate || !EventTemplate->CanPasteHere(Graph))
				{
					return SpawnerFailure(
						Context,
						NodeSpec,
						FString::Printf(
							TEXT("Graph event function '%s' cannot be placed in Blueprint '%s'"),
							*Function->GetPathName(),
							*Context.Blueprint->GetPathName()),
						TEXT("UnsupportedGraphEvent"));
				}
			}
		}
		Spawner = EventSpawner;
	}
	else if (NodeClass->IsChildOf(UK2Node_CallFunction::StaticClass()))
	{
		UFunction* Function = FAssetDocumentK2MemberRefUtils::ResolveMemberFunction(Context.Blueprint, NodeSpec.Member);
		if (!Function)
		{
			return NodeSpec.Member.IsValid()
				? FAssetDocumentK2MemberRefUtils::MakeUnresolvedMemberFailure(Context, NodeSpec, TEXT("UnresolvedGraphFunction"))
				: FAssetDocumentK2MemberRefUtils::MakeMissingMemberFailure(Context, NodeSpec, TEXT("MissingGraphMemberReference"));
		}
		Spawner = UBlueprintFunctionNodeSpawner::Create(NodeClass, Function);
	}
	else if (NodeClass->IsChildOf(UK2Node_Variable::StaticClass()))
	{
		FProperty* Property = FAssetDocumentK2MemberRefUtils::ResolveMemberProperty(Context.Blueprint, NodeSpec.Member);
		if (!Property)
		{
			return NodeSpec.Member.IsValid()
				? FAssetDocumentK2MemberRefUtils::MakeUnresolvedMemberFailure(Context, NodeSpec, TEXT("UnresolvedGraphMemberReference"))
				: FAssetDocumentK2MemberRefUtils::MakeMissingMemberFailure(Context, NodeSpec, TEXT("MissingGraphMemberReference"));
		}
		if (Property->GetOwner<UFunction>())
		{
			return SpawnerFailure(
				Context,
				NodeSpec,
				FString::Printf(TEXT("Graph variable '%s' is local to a function and is outside the current MemberRef scope"), *Property->GetName()),
				TEXT("UnsupportedGraphVariableScope"));
		}

		// A non-null VarContext makes CreateFromMemberOrParam take UE's local-variable path.
		// The current document contract represents class members only.
		Spawner = UBlueprintVariableNodeSpawner::CreateFromMemberOrParam(
			NodeClass,
			Property,
			nullptr,
			Property->GetOwnerClass());
	}
	else if (NodeClass->IsChildOf(UK2Node_Self::StaticClass()))
	{
		Spawner = UBlueprintNodeSpawner::Create(NodeClass);
	}
	else
	{
		return SpawnerFailure(
			Context,
			NodeSpec,
			FString::Printf(TEXT("Graph node class '%s' has no Tier 1 spawner"), *NodeSpec.Class),
			TEXT("UnsupportedGraphNodeClass"),
			true);
	}

	if (!Spawner)
	{
		return SpawnerFailure(
			Context,
			NodeSpec,
			FString::Printf(TEXT("Graph node '%s' spawner could not be created"), *NodeSpec.Id),
			TEXT("UnresolvedGraphNodeSpawner"));
	}

	OutResolved.Spawner.Reset(Spawner);
	OutResolved.ExpectedNodeClass = NodeClass;
	return FAssetDocumentCapabilityResult::Success();
}
