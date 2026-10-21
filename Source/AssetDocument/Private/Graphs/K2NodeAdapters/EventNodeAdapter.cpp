// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/K2NodeAdapters/EventNodeAdapter.h"

#include "Graphs/AssetDocumentGraphEncodingUtils.h"
#include "Graphs/K2NodeAdapters/AssetDocumentK2MemberRefUtils.h"

#include "Engine/Blueprint.h"
#include "K2Node_Event.h"
#include "UObject/Class.h"

FAssetDocumentCapabilityResult FAssetDocumentK2EventNodeAdapter::ConfigureNodeForApply(
	const FAssetDocumentNodeApplyContext& Context,
	UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
	if (!EventNode)
	{
		return FAssetDocumentCapabilityResult::Failure(
			FString::Printf(TEXT("Graph node '%s' is not a K2 event node"), *NodeSpec.Id),
			Context.NodePath,
			TEXT("UnsupportedGraphNodeClass"));
	}

	if (!NodeSpec.Member.IsValid())
	{
		return FAssetDocumentK2MemberRefUtils::MakeMissingMemberFailure(
			Context,
			NodeSpec,
			TEXT("MissingGraphMemberReference"));
	}

	if (!FAssetDocumentK2MemberRefUtils::ResolveMemberFunction(Context.Blueprint, NodeSpec.Member))
	{
		return FAssetDocumentK2MemberRefUtils::MakeUnresolvedMemberFailure(
			Context,
			NodeSpec,
			TEXT("UnresolvedGraphMemberReference"));
	}

	return FAssetDocumentCapabilityResult::Success();
}

bool FAssetDocumentK2EventNodeAdapter::DoesNodeMatchSpec(
	const UBlueprint* Blueprint,
	const UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
	if (!EventNode || Node->GetClass()->GetPathName() != NodeSpec.Class)
	{
		return false;
	}

	UFunction* DesiredFunction = FAssetDocumentK2MemberRefUtils::ResolveMemberFunction(Blueprint, NodeSpec.Member);
	return DesiredFunction && EventNode->FindEventSignatureFunction() == DesiredFunction;
}

bool FAssetDocumentK2EventNodeAdapter::ExtractNode(const UBlueprint* Blueprint, const UK2Node_Event* Node, FAssetDocumentNodeSpec& OutNode) const
{
	if (!Node)
	{
		return false;
	}

	UFunction* Function = Node->FindEventSignatureFunction();
	const UClass* OwnerClass = Node->EventReference.GetMemberParentClass(Blueprint ? Blueprint->SkeletonGeneratedClass : nullptr);
	if (!OwnerClass && Function)
	{
		OwnerClass = Function->GetOwnerClass();
	}

	const FName FunctionName = Node->GetFunctionName();
	if (!OwnerClass || FunctionName.IsNone())
	{
		return false;
	}

	OutNode.Capability = GetCapability();
	OutNode.Member = FAssetDocumentK2MemberRefUtils::MakeMemberRef(
		FAssetDocumentGraphEncodingUtils::GetClassPath(OwnerClass),
		FunctionName,
		Node->EventReference.GetMemberGuid());
	return true;
}
