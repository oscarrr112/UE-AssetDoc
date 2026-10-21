// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/K2NodeAdapters/CallFunctionNodeAdapter.h"

#include "Graphs/AssetDocumentGraphEncodingUtils.h"
#include "Graphs/K2NodeAdapters/AssetDocumentK2MemberRefUtils.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "K2Node_CallFunction.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
FAssetDocumentCapabilityResult UnsupportedFunctionFailure(
	const FAssetDocumentNodeApplyContext& Context,
	const FAssetDocumentNodeSpec& Node,
	const UFunction* Function,
	const FString& Reason)
{
	return FAssetDocumentCapabilityResult::Failure(
		FString::Printf(
			TEXT("Graph call function '%s' is not supported by Tier 1 apply: %s"),
			Function ? *Function->GetPathName() : *Node.Id,
			*Reason),
		Context.NodePath / TEXT("Member"),
		TEXT("UnsupportedGraphFunction"));
}

FAssetDocumentCapabilityResult UnsupportedPinDefaultFailure(
	const FAssetDocumentNodeApplyContext& Context,
	const FAssetDocumentNodeSpec& Node,
	const FAssetDocumentPinOverrideSpec& PinOverride,
	const FString& Reason)
{
	return FAssetDocumentCapabilityResult::Failure(
		FString::Printf(TEXT("Graph call function pin '%s' is not supported by Tier 1 apply: %s"), *PinOverride.Pin, *Reason),
		Context.NodePath / TEXT("PinOverrides") / PinOverride.Pin,
		TEXT("UnsupportedGraphPinDefault"));
}

bool IsSupportedTier1Function(const UFunction* Function, FString& OutReason)
{
	if (!Function)
	{
		OutReason = TEXT("function could not be resolved");
		return false;
	}
	if (Function->HasMetaData(TEXT("Latent")))
	{
		OutReason = TEXT("latent functions require latent action lifecycle handling that Tier 1 graph apply does not author yet");
		return false;
	}
	if (Function->HasMetaData(TEXT("CustomThunk")))
	{
		OutReason = TEXT("custom thunk functions require node-specific expansion that Tier 1 graph apply does not author yet");
		return false;
	}
	if (Function->HasMetaData(TEXT("CustomStructureParam"))
		|| Function->HasMetaData(TEXT("DeterminesOutputType"))
		|| Function->HasMetaData(TEXT("DynamicOutputParam"))
		|| Function->HasMetaData(TEXT("ArrayParm"))
		|| Function->HasMetaData(TEXT("ArrayTypeDependentParams"))
		|| Function->HasMetaData(TEXT("MapParam"))
		|| Function->HasMetaData(TEXT("SetParam")))
	{
		OutReason = TEXT("dynamic, wildcard, or container-dependent function pins are outside Tier 1 graph apply support");
		return false;
	}
	return true;
}

bool IsComplexDefaultProperty(const FProperty* Property)
{
	return Property
		&& (Property->IsA<FArrayProperty>()
			|| Property->IsA<FMapProperty>()
			|| Property->IsA<FSetProperty>()
			|| Property->IsA<FStructProperty>()
			|| Property->IsA<FObjectPropertyBase>()
			|| Property->IsA<FInterfaceProperty>()
			|| Property->IsA<FDelegateProperty>()
			|| Property->IsA<FMulticastDelegateProperty>());
}

FAssetDocumentCapabilityResult PreflightPinDefaultsForFunction(
	const FAssetDocumentNodeApplyContext& Context,
	const FAssetDocumentNodeSpec& Node,
	const UFunction* Function)
{
	for (const FAssetDocumentPinOverrideSpec& PinOverride : Node.PinOverrides)
	{
		const FProperty* Property = Function ? FindFProperty<FProperty>(Function, FName(*PinOverride.Pin)) : nullptr;
		if (PinOverride.DefaultObject.IsValid())
		{
			return UnsupportedPinDefaultFailure(Context, Node, PinOverride, TEXT("DefaultObject overrides are not supported for call-function pins in the current tier"));
		}
		if ((PinOverride.DefaultValue.IsValid() || PinOverride.DefaultTextValue.IsValid()) && IsComplexDefaultProperty(Property))
		{
			return UnsupportedPinDefaultFailure(Context, Node, PinOverride, TEXT("container, struct, object, delegate, and interface defaults are not supported for call-function pins in the current tier"));
		}
	}
	return FAssetDocumentCapabilityResult::Success();
}

bool HasAuthoredDefault(const UEdGraphPin* Pin)
{
	return Pin && (!Pin->DefaultValue.IsEmpty() || Pin->DefaultObject || !Pin->DefaultTextValue.IsEmpty());
}

bool ShouldExtractInputDefault(const UEdGraphPin* Pin)
{
	return Pin
		&& Pin->Direction == EGPD_Input
		&& Pin->LinkedTo.IsEmpty()
		&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
		&& Pin->PinName != UEdGraphSchema_K2::PN_Self
		&& HasAuthoredDefault(Pin);
}

bool IsTruthyString(const FString& Value)
{
	return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("1"), ESearchCase::IgnoreCase);
}

bool IsFalseyString(const FString& Value)
{
	return Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("0"), ESearchCase::IgnoreCase);
}

bool LexicalDefaultsMatch(const FString& Left, const FString& Right)
{
	if (Left == Right)
	{
		return true;
	}

	double LeftNumber = 0.0;
	double RightNumber = 0.0;
	if (LexTryParseString(LeftNumber, *Left) && LexTryParseString(RightNumber, *Right))
	{
		return FMath::IsNearlyEqual(LeftNumber, RightNumber);
	}

	return (IsTruthyString(Left) && IsTruthyString(Right))
		|| (IsFalseyString(Left) && IsFalseyString(Right));
}

bool IsBaselineFunctionPinDefault(const UEdGraphPin* Pin, const UFunction* Function)
{
	if (!Pin || !Function)
	{
		return false;
	}

	if (Pin->DefaultObject)
	{
		return false;
	}

	const FString PinName = Pin->PinName.ToString();
	const FString CppDefaultKey = FString::Printf(TEXT("CPP_Default_%s"), *PinName);
	if (Function->HasMetaData(*CppDefaultKey))
	{
		const FString MetadataDefault = Function->GetMetaData(*CppDefaultKey);
		if (Pin->DefaultValue.IsEmpty() || LexicalDefaultsMatch(Pin->DefaultValue, MetadataDefault))
		{
			return Pin->DefaultTextValue.IsEmpty();
		}
		if (!Pin->DefaultTextValue.IsEmpty() && LexicalDefaultsMatch(Pin->DefaultTextValue.ToString(), MetadataDefault))
		{
			return Pin->DefaultValue.IsEmpty();
		}
		return false;
	}

	return Pin->DefaultValue == TEXT("None") && Pin->DefaultTextValue.IsEmpty();
}
}

FAssetDocumentCapabilityResult FAssetDocumentK2CallFunctionNodeAdapter::ConfigureNodeForApply(
	const FAssetDocumentNodeApplyContext& Context,
	UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
	if (!CallNode)
	{
		return FAssetDocumentCapabilityResult::Failure(
			FString::Printf(TEXT("Graph node '%s' is not a K2 call function node"), *NodeSpec.Id),
			Context.NodePath,
			TEXT("UnsupportedGraphNodeClass"));
	}

	UFunction* Function = FAssetDocumentK2MemberRefUtils::ResolveMemberFunction(Context.Blueprint, NodeSpec.Member);
	if (!Function)
	{
		return NodeSpec.Member.IsValid()
			? FAssetDocumentK2MemberRefUtils::MakeUnresolvedMemberFailure(Context, NodeSpec, TEXT("UnresolvedGraphFunction"))
			: FAssetDocumentK2MemberRefUtils::MakeMissingMemberFailure(Context, NodeSpec, TEXT("MissingGraphMemberReference"));
	}

	FString UnsupportedReason;
	if (!IsSupportedTier1Function(Function, UnsupportedReason))
	{
		return UnsupportedFunctionFailure(Context, NodeSpec, Function, UnsupportedReason);
	}

	const FAssetDocumentCapabilityResult PinSupportResult = PreflightPinDefaultsForFunction(Context, NodeSpec, Function);
	if (!PinSupportResult.bSuccess)
	{
		return PinSupportResult;
	}

	return FAssetDocumentCapabilityResult::Success();
}

bool FAssetDocumentK2CallFunctionNodeAdapter::DoesNodeMatchSpec(
	const UBlueprint* Blueprint,
	const UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec) const
{
	const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
	if (!CallNode || Node->GetClass()->GetPathName() != NodeSpec.Class)
	{
		return false;
	}

	UFunction* DesiredFunction = FAssetDocumentK2MemberRefUtils::ResolveMemberFunction(Blueprint, NodeSpec.Member);
	UFunction* CurrentFunction = CallNode->GetTargetFunction();
	return DesiredFunction && CurrentFunction == DesiredFunction;
}

FAssetDocumentCapabilityResult FAssetDocumentK2CallFunctionNodeAdapter::CanRepresentExistingNode(
	const FAssetDocumentNodeApplyContext& Context,
	const UEdGraphNode* Node) const
{
	const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
	if (!CallNode)
	{
		return FAssetDocumentCapabilityResult::Failure(
			TEXT("Existing graph node is not a K2 call function node"),
			Context.NodePath,
			TEXT("UnsupportedGraphNodeClass"));
	}

	UFunction* Function = CallNode->GetTargetFunction();
	if (!Function)
	{
		Function = CallNode->FunctionReference.ResolveMember<UFunction>(
			Context.Blueprint ? Context.Blueprint->SkeletonGeneratedClass : nullptr,
			true);
	}

	FString UnsupportedReason;
	if (!IsSupportedTier1Function(Function, UnsupportedReason))
	{
		return UnsupportedFunctionFailure(Context, FAssetDocumentNodeSpec(), Function, UnsupportedReason);
	}
	return FAssetDocumentCapabilityResult::Success();
}

bool FAssetDocumentK2CallFunctionNodeAdapter::ExtractNode(const UBlueprint* Blueprint, const UK2Node_CallFunction* Node, FAssetDocumentNodeSpec& OutNode) const
{
	if (!Node)
	{
		return false;
	}

	UFunction* Function = Node->GetTargetFunction();
	if (!Function)
	{
		Function = Node->FunctionReference.ResolveMember<UFunction>(Blueprint ? Blueprint->SkeletonGeneratedClass : nullptr, true);
	}
	if (!Function)
	{
		return false;
	}

	OutNode.Capability = GetCapability();
	OutNode.Member = FAssetDocumentK2MemberRefUtils::MakeMemberRef(
		FAssetDocumentGraphEncodingUtils::GetClassPath(Function ? Function->GetOwnerClass() : nullptr),
		Function ? Function->GetFName() : NAME_None,
		Node->FunctionReference.GetMemberGuid());

	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!ShouldExtractInputDefault(Pin))
		{
			continue;
		}
		if (IsBaselineFunctionPinDefault(Pin, Function))
		{
			continue;
		}

		FAssetDocumentPinOverrideSpec Override;
		Override.Pin = Pin->PinName.ToString();
		if (!Pin->DefaultValue.IsEmpty())
		{
			Override.DefaultValue = MakeShared<FJsonValueString>(Pin->DefaultValue);
		}
		if (Pin->DefaultObject)
		{
			Override.DefaultObject = MakeShared<FJsonValueString>(Pin->DefaultObject->GetPathName());
		}
		if (!Pin->DefaultTextValue.IsEmpty())
		{
			Override.DefaultTextValue = MakeShared<FJsonValueString>(Pin->DefaultTextValue.ToString());
		}
		OutNode.PinOverrides.Add(MoveTemp(Override));
	}
	return true;
}
