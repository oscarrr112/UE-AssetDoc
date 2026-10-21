// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/K2NodeAdapters/VariableGetSetNodeAdapter.h"

#include "Graphs/AssetDocumentGraphEncodingUtils.h"
#include "Graphs/K2NodeAdapters/AssetDocumentK2MemberRefUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
bool IsBlueprintVariable(const UBlueprint* Blueprint, FName VariableName)
{
	return Blueprint && Blueprint->NewVariables.ContainsByPredicate([VariableName](const FBPVariableDescription& Variable)
	{
		return Variable.VarName == VariableName;
	});
}

bool TryMakeVariableMemberRef(const UBlueprint* Blueprint, const UK2Node_Variable* Node, TSharedPtr<FJsonObject>& OutMember)
{
	const FName VariableName = Node ? Node->GetVarName() : NAME_None;
	const FProperty* Property = Node ? Node->GetPropertyForVariable() : nullptr;
	const bool bBlueprintVariable = IsBlueprintVariable(Blueprint, VariableName);
	if ((!bBlueprintVariable && !Property) || (Property && Property->GetOwner<UFunction>()))
	{
		return false;
	}

	const FGuid Guid = Node ? Node->VariableReference.GetMemberGuid() : FGuid();
	OutMember = FAssetDocumentK2MemberRefUtils::MakeMemberRef(
		bBlueprintVariable
			? FString(TEXT("Self"))
			: FAssetDocumentGraphEncodingUtils::GetClassPath(Property ? Property->GetOwnerClass() : nullptr),
		VariableName,
		Guid,
		bBlueprintVariable ? TOptional<bool>(true) : TOptional<bool>());
	return true;
}

template <typename NodeType>
FAssetDocumentCapabilityResult ConfigureVariableNodeForApply(
	const FAssetDocumentNodeApplyContext& Context,
	UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec)
{
	NodeType* VariableNode = Cast<NodeType>(Node);
	if (!VariableNode)
	{
		return FAssetDocumentCapabilityResult::Failure(
			FString::Printf(TEXT("Graph node '%s' is not the expected K2 variable node"), *NodeSpec.Id),
			Context.NodePath,
			TEXT("UnsupportedGraphNodeClass"));
	}

	FString OwnerClassPath;
	FString PropertyName;
	if (!FAssetDocumentK2MemberRefUtils::TryReadMemberRef(NodeSpec.Member, OwnerClassPath, PropertyName))
	{
		return FAssetDocumentK2MemberRefUtils::MakeMissingMemberFailure(
			Context,
			NodeSpec,
			TEXT("MissingGraphMemberReference"));
	}
	if (!FAssetDocumentK2MemberRefUtils::ResolveMemberProperty(Context.Blueprint, NodeSpec.Member))
	{
		return FAssetDocumentK2MemberRefUtils::MakeUnresolvedMemberFailure(
			Context,
			NodeSpec,
			TEXT("UnresolvedGraphMemberReference"));
	}

	return FAssetDocumentCapabilityResult::Success();
}

bool DoesVariableNodeMatchSpec(
	const UBlueprint* Blueprint,
	const UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec)
{
	const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
	if (!VariableNode || Node->GetClass()->GetPathName() != NodeSpec.Class)
	{
		return false;
	}

	FProperty* DesiredProperty = FAssetDocumentK2MemberRefUtils::ResolveMemberProperty(Blueprint, NodeSpec.Member);
	FProperty* CurrentProperty = VariableNode->GetPropertyForVariable();
	if (!DesiredProperty || !CurrentProperty || DesiredProperty->GetFName() != CurrentProperty->GetFName())
	{
		return false;
	}

	const UClass* DesiredOwner = DesiredProperty->GetOwnerClass();
	const UClass* CurrentOwner = CurrentProperty->GetOwnerClass();
	if (!DesiredOwner || !CurrentOwner
		|| DesiredOwner->GetAuthoritativeClass() != CurrentOwner->GetAuthoritativeClass())
	{
		return false;
	}

	const UClass* BlueprintClass = Blueprint
		? (Blueprint->SkeletonGeneratedClass
			? Blueprint->SkeletonGeneratedClass.Get()
			: Blueprint->GeneratedClass
				? Blueprint->GeneratedClass.Get()
				: Blueprint->ParentClass.Get())
		: nullptr;
	const bool bExpectedSelfContext = BlueprintClass
		&& (BlueprintClass->GetAuthoritativeClass() == DesiredOwner->GetAuthoritativeClass()
			|| BlueprintClass->IsChildOf(DesiredOwner));
	if (VariableNode->VariableReference.IsSelfContext() != bExpectedSelfContext)
	{
		return false;
	}

	const FGuid BlueprintVariableGuid = Blueprint
		? FBlueprintEditorUtils::FindMemberVariableGuidByName(
			const_cast<UBlueprint*>(Blueprint),
			DesiredProperty->GetFName())
		: FGuid();
	return !BlueprintVariableGuid.IsValid()
		|| VariableNode->VariableReference.GetMemberGuid() == BlueprintVariableGuid;
}
}

FAssetDocumentCapabilityResult FAssetDocumentK2VariableGetNodeAdapter::ConfigureNodeForApply(
	const FAssetDocumentNodeApplyContext& Context,
	UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	return ConfigureVariableNodeForApply<UK2Node_VariableGet>(Context, Node, NodeSpec);
}

bool FAssetDocumentK2VariableGetNodeAdapter::DoesNodeMatchSpec(
	const UBlueprint* Blueprint,
	const UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	return DoesVariableNodeMatchSpec(Blueprint, Node, NodeSpec);
}

FAssetDocumentCapabilityResult FAssetDocumentK2VariableSetNodeAdapter::ConfigureNodeForApply(
	const FAssetDocumentNodeApplyContext& Context,
	UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	return ConfigureVariableNodeForApply<UK2Node_VariableSet>(Context, Node, NodeSpec);
}

bool FAssetDocumentK2VariableSetNodeAdapter::DoesNodeMatchSpec(
	const UBlueprint* Blueprint,
	const UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	return DoesVariableNodeMatchSpec(Blueprint, Node, NodeSpec);
}

bool FAssetDocumentK2VariableGetNodeAdapter::ExtractNode(const UBlueprint* Blueprint, const UK2Node_VariableGet* Node, FAssetDocumentNodeSpec& OutNode) const
{
	if (!Node || Node->GetVarName().IsNone())
	{
		return false;
	}

	OutNode.Capability = GetCapability();
	return TryMakeVariableMemberRef(Blueprint, Node, OutNode.Member);
}

bool FAssetDocumentK2VariableSetNodeAdapter::ExtractNode(const UBlueprint* Blueprint, const UK2Node_VariableSet* Node, FAssetDocumentNodeSpec& OutNode) const
{
	if (!Node || Node->GetVarName().IsNone())
	{
		return false;
	}

	OutNode.Capability = GetCapability();
	return TryMakeVariableMemberRef(Blueprint, Node, OutNode.Member);
}
